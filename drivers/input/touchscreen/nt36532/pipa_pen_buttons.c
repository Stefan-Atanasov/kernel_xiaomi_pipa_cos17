// SPDX-License-Identifier: GPL-2.0
/*
 * pipa (2026-11-20, BUGS 153): the two buttons of the Xiaomi Smart Pen, one press = one event.
 *
 * The pen sends each button press twice:
 * - through the display: NT36532 pen frames, only while the pen is in hover range. nt36xxx.c reports them as
 *   BTN_STYLUS (front, next to the tip) and BTN_STYLUS2 (back) inside the pen events, so drawing apps get the
 *   stylus buttons together with the pen position;
 * - over Bluetooth: uhid "Xiaomi Smart Pen Keyboard", KEY_PAGEDOWN (front) and KEY_PAGEUP (back), at any distance.
 *
 * This filter owns the Bluetooth keys (nothing else sees them):
 * - a press that starts while the display sees the pen is dropped: the display reports it;
 * - a press that starts with the pen away goes to the virtual device "pipa_pen_buttons". A short press (released
 *   before long_ms) gives KEY_PAGEDOWN/KEY_PAGEUP (far_mode 1, as on HyperOS) or BTN_STYLUS/BTN_STYLUS2
 *   (far_mode 2) or KEY_F21/KEY_F22 (far_mode 3, the ROM runs the chosen short-press actions). A press held for
 *   long_ms gives KEY_F23 (front) / KEY_F24 (back) at that moment: the ROM
 *   (pen_keys.sh) runs the long-press actions (quick note, screenshot). nt36xxx.c does not report a button
 *   while its press belongs to this path (pipa_pen_far_mask).
 * - far_mode 0: the Bluetooth keys are dropped (LineageOS behaviour: display only).
 * HyperOS: MiuiStylusShortcutManager, LONG_PRESS_TIME_OUT 380 ms, long PAGE_UP = screenshot, long PAGE_DOWN =
 * quick note. Public pipa trees ignore the Bluetooth keys (LineageOS sm8250-common 3ea0dc949a).
 */
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include "pipa_pen_buttons.h"

static int far_mode = 1;
module_param(far_mode, int, 0644);
MODULE_PARM_DESC(far_mode, "pen away: 0 = buttons off, 1 = page down/up, 2 = stylus buttons, 3 = custom (F21/F22)");
static int long_ms = 380;
module_param(long_ms, int, 0644);
MODULE_PARM_DESC(long_ms, "hold time of a long press (ms)");
static int range_ms = 150;
module_param(range_ms, int, 0644);
MODULE_PARM_DESC(range_ms, "the pen counts as near for this time after its last display frame (ms)");

enum { B_FRONT, B_BACK, B_NUM };
enum { ST_IDLE, ST_NEAR, ST_FAR, ST_FAR_LONG };

static const unsigned int bt_code[B_NUM] = { KEY_PAGEDOWN, KEY_PAGEUP };
static const unsigned int page_code[B_NUM] = { KEY_PAGEDOWN, KEY_PAGEUP };
static const unsigned int stylus_code[B_NUM] = { BTN_STYLUS, BTN_STYLUS2 };
static const unsigned int custom_code[B_NUM] = { KEY_F21, KEY_F22 };
static const unsigned int long_code[B_NUM] = { KEY_F23, KEY_F24 };

static DEFINE_SPINLOCK(pp_lock);
static unsigned long last_frame;
static bool seen_frame;
static int state[B_NUM];
static struct timer_list long_timer[B_NUM];
static struct input_dev *vdev;

void pipa_pen_frame(void)
{
	WRITE_ONCE(last_frame, jiffies);
	WRITE_ONCE(seen_frame, true);
}

static bool pen_near(void)
{
	return READ_ONCE(seen_frame) &&
		time_before(jiffies, READ_ONCE(last_frame) + msecs_to_jiffies(range_ms));
}

unsigned int pipa_pen_far_mask(void)
{
	unsigned int m = 0;
	int b;

	for (b = 0; b < B_NUM; b++)
		if (READ_ONCE(state[b]) >= ST_FAR)
			m |= BIT(b);
	return m;
}

/* press and release of one key on the virtual device; pp_lock held */
static void vkey(unsigned int code)
{
	if (!vdev)
		return;
	input_report_key(vdev, code, 1);
	input_sync(vdev);
	input_report_key(vdev, code, 0);
	input_sync(vdev);
}

static void long_fire(struct timer_list *t)
{
	int b = (t == &long_timer[B_FRONT]) ? B_FRONT : B_BACK;
	unsigned long flags;

	spin_lock_irqsave(&pp_lock, flags);
	if (state[b] == ST_FAR) {
		state[b] = ST_FAR_LONG;
		vkey(long_code[b]);
	}
	spin_unlock_irqrestore(&pp_lock, flags);
}

static bool pp_filter(struct input_handle *handle, unsigned int type, unsigned int code, int value)
{
	unsigned long flags;
	int b;

	if (type != EV_KEY)
		return false;
	if (code == bt_code[B_FRONT])
		b = B_FRONT;
	else if (code == bt_code[B_BACK])
		b = B_BACK;
	else
		return false;

	spin_lock_irqsave(&pp_lock, flags);
	if (value == 1 && state[b] == ST_IDLE) {
		if (far_mode <= 0 || pen_near()) {
			state[b] = ST_NEAR;
		} else {
			state[b] = ST_FAR;
			mod_timer(&long_timer[b], jiffies + msecs_to_jiffies(long_ms > 0 ? long_ms : 380));
		}
	} else if (value == 0) {
		if (state[b] == ST_FAR) {
			del_timer(&long_timer[b]);
			vkey(far_mode == 3 ? custom_code[b] : far_mode == 2 ? stylus_code[b] : page_code[b]);
		}
		state[b] = ST_IDLE;
	}
	/* value 2 (autorepeat) and a second down: dropped */
	spin_unlock_irqrestore(&pp_lock, flags);
	return true;
}

static int pp_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int err;

	if (!dev->name || !strstr(dev->name, "Smart Pen"))
		return -ENODEV;
	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "pipa_pen_buttons";
	err = input_register_handle(handle);
	if (err)
		goto err_free;
	err = input_open_device(handle);
	if (err)
		goto err_unregister;
	pr_info("pipa_pen_buttons: owns the keys of %s\n", dev->name);
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return err;
}

static void pp_disconnect(struct input_handle *handle)
{
	unsigned long flags;
	int b;

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
	spin_lock_irqsave(&pp_lock, flags);
	for (b = 0; b < B_NUM; b++) {
		del_timer(&long_timer[b]);
		state[b] = ST_IDLE;
	}
	spin_unlock_irqrestore(&pp_lock, flags);
}

static const struct input_device_id pp_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_BUS | INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
		.bustype = BUS_BLUETOOTH,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_PAGEUP)] = BIT_MASK(KEY_PAGEUP) | BIT_MASK(KEY_PAGEDOWN) },
	},
	{ },
};

static struct input_handler pp_handler = {
	.filter = pp_filter,
	.connect = pp_connect,
	.disconnect = pp_disconnect,
	.name = "pipa_pen_buttons",
	.id_table = pp_ids,
};

static int __init pp_init(void)
{
	int b, err;

	vdev = input_allocate_device();
	if (!vdev)
		return -ENOMEM;
	vdev->name = "pipa_pen_buttons";
	vdev->phys = "pipa/pen";
	vdev->id.bustype = BUS_VIRTUAL;
	__set_bit(EV_KEY, vdev->evbit);
	for (b = 0; b < B_NUM; b++) {
		__set_bit(page_code[b], vdev->keybit);
		__set_bit(stylus_code[b], vdev->keybit);
		__set_bit(custom_code[b], vdev->keybit);
		__set_bit(long_code[b], vdev->keybit);
		timer_setup(&long_timer[b], long_fire, 0);
	}
	err = input_register_device(vdev);
	if (err) {
		input_free_device(vdev);
		vdev = NULL;
		return err;
	}
	err = input_register_handler(&pp_handler);
	if (err) {
		input_unregister_device(vdev);
		vdev = NULL;
	}
	return err;
}
late_initcall(pp_init);
