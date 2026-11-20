/** ***************************************************************************
 * @file nano_input.c
 *
 * @brief implent nanosic virtual input driver such as keyboard , mouse and touch
 *
 * <em>Copyright (C) 2010, Nanosic, Inc.  All rights reserved.</em>
 * Author : Bin.yuan bin.yuan@nanosic.com
 * */

/*
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/hid.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "nano_macro.h"
#include "nano_input.h"

static struct hid_device * gVirtualConsumerDev;
static struct hid_device * gVirtualKeyboardDev;
static struct hid_device * gVirtualTouchDev;
static struct hid_device * gVirtualMouseDev;

/*
 * pipa: the devices exist only while the keypad is attached. Registered at probe, the
 * "Xiaomi Keyboard" (an alphabetic external keyboard) told Android a hardware keyboard was
 * always present: physical-keyboard UI and notification, no full on-screen keyboard.
 * Attach/detach come from the keypad hall status (0xA2, bit 0); any keypad report also
 * attaches, so typing works even if no status was seen yet.
 */
static DEFINE_MUTEX(gInputLock);
static bool gInputAttached;
static bool gInputWant;
static struct work_struct gInputWork;

/*
 * pipa: fold state. xiaomi-keyboard (userspace) computes the cover angle from the tablet
 * and keypad accelerometers and writes 32 FF 00 to nanodev0 when the keypad is closed or
 * folded behind the tablet, 32 FF 01 when it is usable again (stock HyperOS does the same
 * in miui-services and disables the input devices). This driver used to drop the write.
 *   gFoldReq  last request of the daemon (true = keypad away)
 *   gFoldOff  applied state. A request to disable applies after fold_debounce_ms without
 *             an enable in between: the daemon flips for a moment while the tablet moves
 *             (power button press, wake). Enable applies at once.
 *   fold_remove=0  the devices stay and their reports are dropped (works with every ROM)
 *   fold_remove=1  the devices are removed, as on detach: Android sees no hardware
 *             keyboard, so the on-screen keyboard returns. Needs a xiaomi-keyboard that
 *             does not use /dev/input/event12 as its "keypad present" flag.
 */
static bool gFoldReq;
static bool gFoldOff;
static bool gPanelOn = true;
static struct delayed_work gFoldWork;
static bool fold_remove;
module_param(fold_remove, bool, 0644);
static unsigned int fold_debounce_ms = 1000;
module_param(fold_debounce_ms, uint, 0644);
module_param_named(fold_off, gFoldOff, bool, 0444);

/*
 * pipa: /proc/nanokbd exists while the keypad is attached (MCU hall status or a keypad
 * report), whatever the fold state and the event numbers. A xiaomi-keyboard patched to
 * access() it instead of /dev/input/event12 samples the accelerometer exactly while a
 * keypad is there: no idle drain without one, and it can see the unfold after the input
 * devices were removed. Called with gInputLock held.
 */
static bool gPresent;

static int
Nanosic_input_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "attached=%d input=%d fold_req=%d fold_off=%d fold_remove=%d debounce_ms=%u\n",
		gInputWant, gInputAttached, gFoldReq, gFoldOff, fold_remove, fold_debounce_ms);
	return 0;
}

static void
Nanosic_input_present(bool present)
{
	if(gPresent == present)
		return;
	if(!present)
		remove_proc_entry("nanokbd", NULL);
	else if(!proc_create_single("nanokbd", 0444, NULL, Nanosic_input_proc_show))
		return;
	gPresent = present;
}
static int Nanosic_input_attach(void);
static void Nanosic_input_detach(void);

/*定义虚拟化的键盘,鼠标,触摸设备*/
static struct nano_input_create_dev gConsumerDevice =
{
    .name = "Xiaomi Consumer",
    .rd_data = HID_ConsumerReportDescriptor,
    .rd_size = sizeof(HID_ConsumerReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA4,
    .bus     = BUS_VIRTUAL,
};

static struct nano_input_create_dev gKeyboardDevice =
{
    .name = "Xiaomi Keyboard",
    .rd_data = HID_KeyboardReportDescriptor,
    .rd_size = sizeof(HID_KeyboardReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA3,
    .bus     = BUS_VIRTUAL,
};
/*鼠标通道*/
static struct nano_input_create_dev gMouseDevice =
{
    .name = "Xiaomi Mouse",
    .rd_data = HID_MouseReportDescriptor,
    .rd_size = sizeof(HID_MouseReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA2,
    .bus     = BUS_VIRTUAL,
};
/*触摸通道*/
static struct nano_input_create_dev gTouchDevice =
{
    .name = "Xiaomi Touch",
    .rd_data = HID_TouchReportDescriptor,
    .rd_size = sizeof(HID_TouchReportDescriptor),
    .vendor  = 0x15D9,
    .product = 0xA1,
    .bus     = BUS_VIRTUAL,
};

/** **************************************************************************
 * @brief Nanosic_input_write
 *        data inject to input layer , 键盘,鼠标,多指数据的注入
 ** */
int
Nanosic_input_write(EM_PacketType type ,void *buf, size_t len)
{
    int ret = -1;

    if(type > EM_PACKET_TOUCH)
        return 0;

    mutex_lock(&gInputLock);
    if(gFoldOff){
        /* folded away: no report, and no attach from a key pressed against the table */
        mutex_unlock(&gInputLock);
        return 0;
    }
    if(!gInputAttached){
        if(Nanosic_input_attach() < 0){
            mutex_unlock(&gInputLock);
            return 0;
        }
        Nanosic_input_present(true);
    }
	switch(type)
	{
		case EM_PACKET_KEYBOARD:
            //dbgprint(DEBUG_LEVEL,"report keyboard event\n");
            ret = hid_input_report(gVirtualKeyboardDev, HID_INPUT_REPORT, buf,min_t(size_t, len, 100), 0);
            if(ret)
                dbgprint(ERROR_LEVEL,"Nanosic_input_write：report keyboard event err: %d \n",ret);
			break;

        case EM_PACKET_CONSUMER:
            ret = hid_input_report(gVirtualConsumerDev, HID_INPUT_REPORT, buf,min_t(size_t, len, 100), 0);
            if(ret)
                dbgprint(ERROR_LEVEL,"Nanosic_input_write：report keyboard event err: %d \n",ret);
            break;

		case EM_PACKET_MOUSE:
            //dbgprint(DEBUG_LEVEL,"report mouse event \n");
            hid_input_report(gVirtualMouseDev, HID_INPUT_REPORT, buf,min_t(size_t, len, 100), 0);
			break;

		case EM_PACKET_TOUCH:
            //dbgprint(DEBUG_LEVEL,"report touch event\n");
            hid_input_report(gVirtualTouchDev, HID_INPUT_REPORT, buf,min_t(size_t, len, 100), 0);
			break;

		default:
			break;
	}
    mutex_unlock(&gInputLock);

	return 0;
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_start
 *
 *  @brief
 *
 ** */
static int
Nanosic_input_hid_start(struct hid_device *hid)
{
    dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_start ****\n");
    return 0;
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_stop
 *
 *  @brief
 *
 ** */
static void
Nanosic_input_hid_stop(struct hid_device *hid)
{
    hid->claimed = 0;
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_stop ****\n");
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_open
 *
 *  @brief
 *
 ** */
static int
Nanosic_input_hid_open(struct hid_device *hid)
{
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_open ****\n");
    return 0;
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_close
 *
 *  @brief
 *
 ** */
static void
Nanosic_input_hid_close(struct hid_device *hid)
{
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_close ****\n");
}

/** ************************************************************************//**
 *  @func Nanosic_input_hid_parse
 *
 *  @brief  OK
 *
 ** */
static int
Nanosic_input_hid_parse(struct hid_device *hid)
{
	struct nano_input_create_dev* ev = hid->driver_data;
    //dbgprint(DEBUG_LEVEL,"**** Nanosic_input_hid_parse ****\n");
	return hid_parse_report(hid, ev->rd_data, ev->rd_size);
}

/** ************************************************************************//**
 *  @func Nanosic_input_raw_request
 *
 *  @brief support send raw report to hidraw device
 *
 ** */
static int
Nanosic_input_raw_request (struct hid_device *hdev, unsigned char reportnum,
                                        __u8 *buf, size_t len, unsigned char rtype,int reqtype)
{
    return len;
}

/*
 * pipa (2026-11-20, BUGS 151): caps lock LED. hid-input sends the keyboard LED output report here
 * (report ID 5, byte 1 bit 1 = caps lock). HyperOS lights the LED from miui-services
 * (IICNodeHelper.setKeyboardFeature: MCU feature 0x26, value 1/0; FD/FC only for keyboard type 33,
 * the touchpad keyboard) through the vendor nano socket; the port has no such framework, so the
 * driver sends the same MCU command: 32 00 4E 31 80 38 26 01 <value> <sum of bytes 2..8>.
 * Like HyperOS the LED is off while the keypad is folded away. caps_led_on/off: values to send.
 */
static int caps_led_on = 0x01;
module_param(caps_led_on, int, 0644);
static int caps_led_off = 0x00;
module_param(caps_led_off, int, 0644);
static bool gCapsLock;
static struct delayed_work gCapsWork;

static void
Nanosic_input_caps_send(bool on)
{
	u8 cmd[I2C_DATA_LENGTH_WRITE] = { 0x32, 0x00, 0x4E, 0x31, 0x80, 0x38, 0x26, 0x01 };
	u8 sum = 0;
	int i;

	cmd[8] = (u8)(on ? caps_led_on : caps_led_off);
	for(i = 2; i < 9; i++)
		sum += cmd[i];
	cmd[9] = sum;
	if(Nanosic_i2c_write(gI2c_client, cmd, sizeof(cmd)) < 0)
		dbgprint(ERROR_LEVEL, "caps lock led %d: i2c write failed\n", on);
	else
		dbgprint(ALERT_LEVEL, "caps lock led %d\n", on);
}

/* the LED as the keypad should show it now (keyboard device present, not folded away) */
static void
Nanosic_input_caps_work(struct work_struct *work)
{
	mutex_lock(&gInputLock);
	if(gInputAttached)
		Nanosic_input_caps_send(gCapsLock && !gFoldOff);
	mutex_unlock(&gInputLock);
}

/** ************************************************************************//**
 *  @func Nanosic_input_set_report
 *
 *  @brief support set report to i2c slave device
 *
 ** */
static int
Nanosic_input_set_report(struct hid_device *hid, __u8 *buf, size_t size)
{

    if(IS_ERR(hid)){
        dbgprint(ERROR_LEVEL, "Invaild argment\n");
        return -EINVAL;
    }

    if(size <= 0){
        dbgprint(ERROR_LEVEL, "Invaild argment\n");
        return -EINVAL;
    }

    if(IS_ERR(buf)){
        dbgprint(ERROR_LEVEL, "Invaild argment\n");
        return -EINVAL;
    }

    if(size >= 2 && buf[0] == 0x05 && hid == gVirtualKeyboardDev){
        /* keyboard LED output report: caps lock is bit 1 */
        gCapsLock = !!(buf[1] & 0x02);
        mod_delayed_work(system_wq, &gCapsWork, 0);
        return size;
    }

    rawdata_show("would not write i2c cmd" ,buf , size);
    return -EINVAL;

    /*return Nanosic_i2c_write(gI2c_client,buf , size);*/
}

/** ************************************************************************//**
 *  @func Nanosic_hid_ll_driver
 *
 *  @brief
 *
 ** */
static struct hid_ll_driver Nanosic_hid_ll_driver = {
	.start = Nanosic_input_hid_start,       /*call on probe dev*/
	.stop  = Nanosic_input_hid_stop,        /*call on remove dev*/
	.open  = Nanosic_input_hid_open,        /*call on input layer open*/
	.close = Nanosic_input_hid_close,       /*call on i input layer close*/
	.parse = Nanosic_input_hid_parse,       /*copy report map description*/
	.raw_request = Nanosic_input_raw_request,
	.output_report = Nanosic_input_set_report,
};

/** ************************************************************************//**
 *  @func Nanosic_input_create
 *
 *  @brief
 *
 ** */
static struct hid_device *
Nanosic_input_create(struct nano_input_create_dev* ev)
{
	struct hid_device *hid = NULL;
	int ret;

    hid = hid_allocate_device();
    if (IS_ERR_OR_NULL(hid)) {
        ret = PTR_ERR(hid);
        return NULL;
    }

    strncpy(hid->name, ev->name, 127);
    hid->name[127] = 0;
    strncpy(hid->phys, ev->phys, 63);
    hid->phys[63] = 0;
    strncpy(hid->uniq, ev->uniq, 63);
    hid->uniq[63] = 0;

    hid->ll_driver = &Nanosic_hid_ll_driver;
    hid->bus = ev->bus;
    hid->vendor = ev->vendor;
    hid->product = ev->product;
    hid->version = ev->version;
    hid->country = ev->country;
    hid->driver_data = ev;
    hid->dev.parent = NULL;

    ret = hid_add_device(hid);
    if (ret) {
        hid_err(hid, "Cannot register HID device\n");
        goto err_hid;
    }

    return hid;

err_hid:

	hid_destroy_device(hid);
    return NULL;
}

/** ************************************************************************//**
 *  @func Nanosic_input_register
 *
 *  @brief register hid virtual device { 注册对应的input 虚拟设备 }
 *
 ** */
static int
Nanosic_input_attach(void)
{
	gVirtualKeyboardDev = Nanosic_input_create(&gKeyboardDevice);
	if(IS_ERR_OR_NULL(gVirtualKeyboardDev))
		goto _err1;

	gVirtualMouseDev = Nanosic_input_create(&gMouseDevice);
	if(IS_ERR_OR_NULL(gVirtualMouseDev))
		goto _err2;

	gVirtualTouchDev = Nanosic_input_create(&gTouchDevice);
	if(IS_ERR_OR_NULL(gVirtualTouchDev))
		goto _err3;

	gVirtualConsumerDev = Nanosic_input_create(&gConsumerDevice);
	if(IS_ERR_OR_NULL(gVirtualConsumerDev))
		goto _err4;
	gInputAttached = true;
	dbgprint(ALERT_LEVEL,"keypad attached, input create ok\n");

	return 0;
_err4:
	hid_destroy_device(gVirtualTouchDev);
_err3:
	hid_destroy_device(gVirtualMouseDev);
_err2:
	hid_destroy_device(gVirtualKeyboardDev);
_err1:
	gVirtualConsumerDev = gVirtualTouchDev = gVirtualMouseDev = gVirtualKeyboardDev = NULL;
	dbgprint(ERROR_LEVEL,"input create err\n");
	return -1;
}

static void
Nanosic_input_detach(void)
{
	if(!gInputAttached)
		return;

	hid_destroy_device(gVirtualKeyboardDev);
	hid_destroy_device(gVirtualMouseDev);
	hid_destroy_device(gVirtualTouchDev);
	hid_destroy_device(gVirtualConsumerDev);
	gVirtualConsumerDev = gVirtualTouchDev = gVirtualMouseDev = gVirtualKeyboardDev = NULL;
	gInputAttached = false;
	dbgprint(ALERT_LEVEL,"keypad detached, input release ok\n");
}

static void
Nanosic_input_work(struct work_struct *work)
{
	mutex_lock(&gInputLock);
	Nanosic_input_present(gInputWant);
	if(!gInputWant)
		Nanosic_input_detach();
	else if(!gInputAttached && !gFoldOff)
		Nanosic_input_attach();
	mutex_unlock(&gInputLock);
}

/* the debounce time passed without an enable: apply the fold */
static void
Nanosic_input_fold_work(struct work_struct *work)
{
	static const u8 kbd_up[9] = { 0x05 };
	static const u8 consumer_up[5] = { 0x06 };
	u8 buf[9];

	mutex_lock(&gInputLock);
	if(gFoldReq && !gFoldOff && gPanelOn){
		gFoldOff = true;
		if(gInputAttached && gCapsLock)
			Nanosic_input_caps_send(false);
		if(gInputAttached){
			if(fold_remove){
				Nanosic_input_detach();
			}else{
				/* release what is held: a key down now would repeat for ever */
				memcpy(buf, kbd_up, sizeof(kbd_up));
				hid_input_report(gVirtualKeyboardDev, HID_INPUT_REPORT, buf, sizeof(kbd_up), 0);
				memcpy(buf, consumer_up, sizeof(consumer_up));
				hid_input_report(gVirtualConsumerDev, HID_INPUT_REPORT, buf, sizeof(consumer_up), 0);
			}
		}
		dbgprint(ALERT_LEVEL,"keypad folded away, input %s\n", fold_remove ? "removed" : "muted");
	}
	mutex_unlock(&gInputLock);
}

/** ************************************************************************//**
 *  @func Nanosic_input_set_enabled
 *
 *  @brief fold request from xiaomi-keyboard (32 FF 00/01), or true when it closes nanodev0
 *
 ** */
void
Nanosic_input_set_enabled(bool enabled)
{
	mutex_lock(&gInputLock);
	gFoldReq = !enabled;
	if(enabled){
		cancel_delayed_work(&gFoldWork);
		if(gFoldOff){
			gFoldOff = false;
			if(gInputWant && !gInputAttached)
				Nanosic_input_attach();
			if(gCapsLock)
				mod_delayed_work(system_wq, &gCapsWork, 0);
			dbgprint(ALERT_LEVEL,"keypad unfolded, input on\n");
		}
	}else if(!gFoldOff && gPanelOn){
		/* no-op while one is pending: repeated requests do not push the deadline */
		schedule_delayed_work(&gFoldWork, msecs_to_jiffies(fold_debounce_ms));
	}
	mutex_unlock(&gInputLock);
}

/** ************************************************************************//**
 *  @func Nanosic_input_panel
 *
 *  @brief screen state. The keypad MCU sleeps with the screen and the daemon keeps its last
 *         answer, often a false "away" from pressing the power button: nothing is applied
 *         while the screen is off; after the wake a still pending request counts again.
 *
 ** */
void
Nanosic_input_panel(bool on)
{
	mutex_lock(&gInputLock);
	gPanelOn = on;
	/* the keypad MCU sleeps with the screen: give the caps lock LED back after the wake */
	if(on && gCapsLock)
		mod_delayed_work(system_wq, &gCapsWork, msecs_to_jiffies(500));
	if(!on)
		cancel_delayed_work(&gFoldWork);
	else if(gFoldReq && !gFoldOff)
		schedule_delayed_work(&gFoldWork, msecs_to_jiffies(fold_debounce_ms));
	mutex_unlock(&gInputLock);
}

/** ************************************************************************//**
 *  @func Nanosic_input_hall_status
 *
 *  @brief keypad hall status from the MCU: create or remove the input devices
 *
 ** */
void
Nanosic_input_hall_status(bool connected)
{
	if(gInputWant == connected)
		return;
	gInputWant = connected;
	schedule_work(&gInputWork);
}

/** ************************************************************************//**
 *  @func Nanosic_input_register
 *
 *  @brief the devices are created on keypad attach { 键盘接入时才注册input虚拟设备 }
 *
 ** */
int
Nanosic_input_register(void)
{
	INIT_WORK(&gInputWork, Nanosic_input_work);
	INIT_DELAYED_WORK(&gFoldWork, Nanosic_input_fold_work);
	INIT_DELAYED_WORK(&gCapsWork, Nanosic_input_caps_work);
	return 0;
}

/** ************************************************************************//**
 *  @func Nanosic_input_release
 *
 *  @brief
 *
 ** */
int
Nanosic_input_release(void)
{
	cancel_work_sync(&gInputWork);
	cancel_delayed_work_sync(&gFoldWork);
	mutex_lock(&gInputLock);
	Nanosic_input_detach();
	Nanosic_input_present(false);
	mutex_unlock(&gInputLock);

	return 0;
}

