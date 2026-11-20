# Kernel for the ColorOS 17 port on Xiaomi Pad 6 (pipa)

Kernel `walt-hi13` (Linux 4.19.325; port release v9.2) for the ColorOS 17 port on the Xiaomi Pad 6 (`pipa`, SM8250 / SD870). `walt-r6` is
the beta5 release. r7: keypad input devices only while the keypad is attached (nanosic), no NV MAC wait for Wi-Fi
(cnss2), `ovnet` virtual netdev for OPPO interconnect network sharing. r8: SukiSU-Ultra builtin `85d3b048` (v4.3.0
level, manager 41100) with 4.19 compat, susfs priority-uid-filter backport. r8kpm: r8 + KPM (KernelPatch 0.14.0,
patched into the Image after the build, see below). r9: r8kpm + NT36532 touch charger mode (the driver tells the touch
firmware the USB state: 0x53 plug in, 0x51 out; no touch lag while charging). r10: r9 + `/proc/oplus_binder/ux_flag`
(accepts the ColorOS 16+ binder UX ioctl as a no-op; stops the SchedAssist "fd invalid" log spam).
r11: ColorOS 16+ IM_FLAG table (11 forbid affinity ... 18 render thread) and the IM_FLAG_CLEAR form (64+N clears N;
before, threads kept a literal 70/74 and the system_server leader lost its flag). r12: OPPO `qos_sched`
(`/proc/oplus_qos_sched`, ioctl ABI of ColorOS `qos_sched.so`) for the ColorOS QoS scheduler (`ro.oplus.osense.uaf_qos_enable`).
r13: qos_sched hardening (LUT lifetime per mapping, zeroed pages, locking, system-only nodes) and a no-op
`cpu.schedtune.boost` cgroup file (the ColorOS group setup writes it last and failed without it).
r14: DisplayPort (USB-C monitor) at most 8 bpc by default: a GO27Q24 dropped the link (HPD low) at 10 bpc; module parameters `pipa_dp_hdcp`, `pipa_dp_max_bpc`, `pipa_dp_log` in `/sys/module/msm_drm/parameters/`.
r15: the DP preferred mode is the highest refresh rate at the monitor's preferred resolution that fits the trained link (SurfaceFlinger keeps an external display at its first mode, e.g. 2560x1440@120 instead of @60); `pipa_dp_prefer_max_rate`, off until reboot after two quick HPD drops.
r16: `/proc/shell-temp` for ColorOS `horae` (port of OPPO `horae_shell_temp`: horae writes "<index> <m°C>", a read returns the highest value; no thermal zone on pipa, so the Xiaomi/Qualcomm thermal handling is unchanged).
r17: SukiSU-Ultra builtin `1cfa5734` (v4.3.0-r1 level, kernel version 41140, uapi 6; the v4.3.0-r1 manager and its ksud match). susfs stays v2.3.0. beta7 ships r17.
r18-r22: NT36532 touch fixes (lock for the pen and USB commands; resume always enables the IRQ and the active pin
state; a failed resume firmware download is retried after the panel is on; full panel power cycle if the firmware stays
dead), nanosic keyboard cover fixes (fold request of xiaomi-keyboard, `/proc/nanokbd`; screen-off key cache drained in
process context). r23: OPPO memory path from the stock OPPO Pad 1 kernel (same SoC): `OPLUS_ZRAM_OPT` (swappiness up to
200, `direct_swappiness`, `/proc/oplus_healthinfo/swappiness_para`), `OPLUS_MULTI_KSWAPD`, `KSWAPD_UNBIND_MAX_CPU`.
r24: OPPO hybridswap (`HYBRIDSWAP`, `_SWAPD`, `_CORE`: memcg-aware zram, `hybridswapd`, eswap to a loop file on
/data = ColorOS RAM expansion; 7 Android vendor hooks, `android_oem_data1` in memcg and pgdat; `ZRAM_WRITEBACK` off as
on stock). r26: hybridswapd stays idle while the panel is off, as on stock (pipa sends panel events through `mi_drm`).
hi1: healthinfo-lite (`/proc/oplus_healthinfo/{cpu_loading,kswapd_loading}`, `/proc/fg_info/fg_uids` for ColorOS
services). hi3: `sched_assist` GPU-composition scene always on (`gpu_comp_static=1`), so the stock perf HAL cannot move
SurfaceFlinger to the little cores. hi5: NT36532 resends the touch features (Game Space Ultra touch, game sliders, edge,
orientation) and restores the probe touch state after each resume firmware download. hi8: after a refresh-rate drop
(dfps 90 -> 60) the display keeps maximum MDP clock and bandwidth for 8 frames / 200 ms (fixes SDE underruns = lines
across the screen with O+ Connect PC Extend); `pipa_vrr_boost`, `pipa_vrr_log` in `/sys/module/msm_drm/parameters/`.
hi9: SukiSU Nova builtin `4bec532d` (renamed SukiSU-Ultra, kernel version 30691, manager `com.sukisu.nova` v3.0.0)
with 4.19 fixes; susfs v2.3.0. The port release v9 (2026-11-19) ships hi9.
hi10: SDE refresh boost on every vrefresh change, also on a rise (60 -> 90 Hz MDP underrun = white flash on some
Tianma panels). hi11: keyboard cover Caps Lock LED from the HID LED report (MCU feature 0x26 as HyperOS), off before
the keypad MCU sleeps at screen off. hi12: pen buttons once: display path = BTN_STYLUS/2 in the pen events, Bluetooth
keys filtered by distance; far_mode 3 = custom short / double press actions (KEY_F19-F22), short press also KEY_F13/F14
(OPPO pencil click). hi13: keyboard Super key alone -> KEY_F18 on pipa_pen_buttons (super_alone; Super shortcuts
stay). The port release v9.2 (2026-11-21) ships hi13.
Branch `oppo` is what the port ships; branch `own` is the same kernel without OPPO's UI First scheduler code.

## What it is made of

| Part | Source |
|---|---|
| Core kernel (WALT, BPF backports, f2fs, mm, net, binder, drm core) | LineageOS `android_kernel_xiaomi_sm8250`, branch `lineage-24.0` |
| pipa device drivers (charger, fuel gauge, pen charger, amps, haptics, camera, dts) | liyafe1997 `kernel_xiaomi_sm8250_mod` (`android15-lineage22-mod`); his original README: `README.liyafe.md` |
| Display (`techpack/display`) + touch | E404R kernel (kvsnr113) |
| Root | SukiSU-Ultra branch `builtin` `85d3b048` (v4.3.0 level, manager 41100; manual 4.19 hooks, no kprobes) + susfs 2.3.0 (+ priority uid filter backport) + optional KPM (KernelPatch 0.14.0) |
| ColorOS "UI First" (sched_assist, frame boost, `/proc/oplus_scheduler`, `/proc/sys/fbg`) | OPPO's open-source sm8250 kernel + modules (`oppo/sm8250_u_14.0.0_oslo`), ColorOS 17 `ux_task` interface ported from sm8450 |
| ColorOS "Hans" app freezer channel (netlink 29; binder, binder_alloc, signal and network reports; `CONFIG_OPLUS_HANS`) | the same OPPO sm8250 kernel (OPD2101). The `hans` daemon is exempt from the 5.10 uname spoof, so it picks this legacy channel |

Changes of our own (see `git log`): WALT frequency/colocation wired to uclamp; SurfaceFlinger GPU contexts on the top
kgsl ringbuffer; SurfaceFlinger + RenderEngine kept off the little cores in a sched_assist scene (`is_sf()`);
`kernel.ux_prefer_big` (heavy UX UI threads on the big cluster without OPPO's vendor daemon), `kernel.frame_rate`
default 120; pstore, boot watchdog (`CONFIG_PIPA_BOOTDBG`), uname spoof for the A17 BPF loaders, config fragment
`arch/arm64/configs/vendor/xiaomi/pipa-cos17.config` (+ `pipa-oplus.config`).

## Needs

ROM beta5-2i or newer (SELinux labels for the UI First nodes); beta5-2l+ sets the SurfaceFlinger scene at boot.
SukiSU manager v4.3.0 (41100). Older ROMs boot but ColorOS gets EACCES on the UI First nodes.

## Build

    ./build-pipa-cos17.sh                     # CLANG=/path/to/clang-r547379 (AOSP clang 20); DBG=1 = boot watchdog
    ./pack-boot.sh out/arch/arm64/boot/Image <a pipa boot.img> boot-new.img   # needs magiskboot in PATH

Config = `kona-perf_defconfig` + `xiaomi/sm8250-common.config` + `xiaomi/pipa.config` + `xiaomi/pipa-cos17.config`
(+ `xiaomi/pipa-oplus.config` on branch `oppo`), merged by `merge_config.sh` in the script; KernelSU/susfs options
are switched on there. Never repack with `magiskboot repack` alone: it rewrites the header's os_version/patch level
and the bootloader then refuses the kernel without any log (black screen); `pack-boot.sh` restores those 4 bytes.

Test without writing anything: `fastboot boot` a trimmed image (`TRIM=1 ./pack-boot.sh ...`; the full 196 MB image
hangs `fastboot boot`). Flash: `fastboot flash boot_b boot-new.img`.

KPM (walt-r8kpm): patch the built Image with KernelPatch before you pack it:

    kptools-linux -p -i out/arch/arm64/boot/Image -k kpimg -s <your superkey> -o Image-kpm
    kptools-linux -l -i Image-kpm             # must show patched=true

Then gzip the Image, so that the trimmed test image stays below the ~92 MB `fastboot boot` limit (a bigger image hangs
the bootloader; `fastboot reboot` recovers). Known problem: a live `ksud sepolicy patch` on r8kpm hung the big cores
(watchdog reset). Put SELinux rule changes into the ROM instead.
