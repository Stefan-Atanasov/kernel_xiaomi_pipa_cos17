# Kernel for the ColorOS 17 port on Xiaomi Pad 6 (pipa)

Kernel `walt-r7ov` (Linux 4.19.325) for the ColorOS 17 port on the Xiaomi Pad 6 (`pipa`, SM8250 / SD870). `walt-r6` is
the beta5 release. r7: keypad input devices only while the keypad is attached (nanosic), no NV MAC wait for Wi-Fi
(cnss2), `ovnet` virtual netdev for OPPO interconnect network sharing.
Branch `oppo` is what the port ships; branch `own` is the same kernel without OPPO's UI First scheduler code.

## What it is made of

| Part | Source |
|---|---|
| Core kernel (WALT, BPF backports, f2fs, mm, net, binder, drm core) | LineageOS `android_kernel_xiaomi_sm8250`, branch `lineage-24.0` |
| pipa device drivers (charger, fuel gauge, pen charger, amps, haptics, camera, dts) | liyafe1997 `kernel_xiaomi_sm8250_mod` (`android15-lineage22-mod`); his original README: `README.liyafe.md` |
| Display (`techpack/display`) + touch | E404R kernel (kvsnr113) |
| Root | SukiSU-Ultra (builtin, uapi 6, manager 4.2.1-r1) + susfs 2.3.0 |
| ColorOS "UI First" (sched_assist, frame boost, `/proc/oplus_scheduler`, `/proc/sys/fbg`) | OPPO's open-source sm8250 kernel + modules (`oppo/sm8250_u_14.0.0_oslo`), ColorOS 17 `ux_task` interface ported from sm8450 |
| ColorOS "Hans" app freezer channel (netlink 29; binder, binder_alloc, signal and network reports; `CONFIG_OPLUS_HANS`) | the same OPPO sm8250 kernel (OPD2101). The `hans` daemon is exempt from the 5.10 uname spoof, so it picks this legacy channel |

Changes of our own (see `git log`): WALT frequency/colocation wired to uclamp; SurfaceFlinger GPU contexts on the top
kgsl ringbuffer; SurfaceFlinger + RenderEngine kept off the little cores in a sched_assist scene (`is_sf()`);
`kernel.ux_prefer_big` (heavy UX UI threads on the big cluster without OPPO's vendor daemon), `kernel.frame_rate`
default 120; pstore, boot watchdog (`CONFIG_PIPA_BOOTDBG`), uname spoof for the A17 BPF loaders, config fragment
`arch/arm64/configs/vendor/xiaomi/pipa-cos17.config` (+ `pipa-oplus.config`).

## Needs

ROM beta5-2i or newer (SELinux labels for the UI First nodes); beta5-2l+ sets the SurfaceFlinger scene at boot.
SukiSU manager 4.2.1-r1. Older ROMs boot but ColorOS gets EACCES on the UI First nodes.

## Build

    ./build-pipa-cos17.sh                     # CLANG=/path/to/clang-r547379 (AOSP clang 20); DBG=1 = boot watchdog
    ./pack-boot.sh out/arch/arm64/boot/Image <a pipa boot.img> boot-new.img   # needs magiskboot in PATH

Config = `kona-perf_defconfig` + `xiaomi/sm8250-common.config` + `xiaomi/pipa.config` + `xiaomi/pipa-cos17.config`
(+ `xiaomi/pipa-oplus.config` on branch `oppo`), merged by `merge_config.sh` in the script; KernelSU/susfs options
are switched on there. Never repack with `magiskboot repack` alone: it rewrites the header's os_version/patch level
and the bootloader then refuses the kernel without any log (black screen); `pack-boot.sh` restores those 4 bytes.

Test without writing anything: `fastboot boot` a trimmed image (`TRIM=1 ./pack-boot.sh ...`; the full 196 MB image
hangs `fastboot boot`). Flash: `fastboot flash boot_b boot-new.img`.
