# DP test build for the GO27Q24 monitor (bug 54)

Branch `cloud/dp-test` (from `oppo` walt-r13, `3e39c1845`). Test build only. Do not merge into `oppo` without a test on
the tablet.

## Problem

USB-C DP to a GO27Q24 monitor (2560x1440, 10-bit, 240 Hz OLED). Link training works and the stream starts
(2560x1440@60, 30 bpp, audio). About 2 s later the monitor drops HPD after repeated USB-PD Attention VDMs. Just before
that, HDCP logs `key_verify -4008`. Suspected cause: the HDCP failure gives CP_IRQ, and CP_IRQ gives a link reset loop.

## What changed

All changes are in `techpack/display/msm/dp/` (DP only). The config and the build script are not changed.

| File | Change |
|---|---|
| `dp_debug.h` | Declarations of the three switches and the macro `PIPA_DP_LOG` (`pr_info` with the tag `msm-dp-pipa`, only when `pipa_dp_log` = 1). |
| `dp_display.c` | Definitions of the switches (module parameters). HDCP is not started when `pipa_dp_hdcp` = 0: `dp_display_update_hdcp_info` selects no HDCP version and `dp_display_check_source_hdcp_caps` sets no source capability. So `hdcp.ops` stays NULL, and the HDCP ISR, CP_IRQ handler and HDCP worker do nothing. Logs for each HPD event (with the decision: connect, attention or disconnect), USB-PD configure and disconnect, each attention (sink request bits, or the CP_IRQ path with the HDCP state), and the chosen mode (resolution, refresh, pixel clock, bpp, sink bpc, lanes, link rate). |
| `dp_panel.c` | `dp_panel_get_supported_bpp`: when `pipa_dp_max_bpc` is not 0, the bpp is at most `3 x pipa_dp_max_bpc` (never below the stock minimum). |
| `dp_ctrl.c` | Logs before link training (lanes, link rate) and after a successful training (lanes, rate, bw code, voltage and pre-emphasis levels). |

## Switches

The switches are module parameters of `msm_drm`. They need no kernel command line. Change them at run time as root.
A new value applies at the next connect, so unplug and plug in the cable after a change.

| Parameter | Test default | Stock behaviour | Effect |
|---|---|---|---|
| `pipa_dp_hdcp` | 0 | 1 | 0 = HDCP 1.x and 2.2 never start on DP. |
| `pipa_dp_max_bpc` | 8 | 0 | 8 = at most 8 bpc (24 bpp). 0 = no cap (the sink EDID decides, up to 30 bpp). |
| `pipa_dp_log` | 1 | 0 | 1 = extra `msm-dp-pipa` lines in dmesg. |

```sh
adb shell "su -c 'cat /sys/module/msm_drm/parameters/pipa_dp_hdcp'"
adb shell "su -c 'echo 1 > /sys/module/msm_drm/parameters/pipa_dp_hdcp'"     # HDCP back on
adb shell "su -c 'echo 0 > /sys/module/msm_drm/parameters/pipa_dp_max_bpc'"  # 10 bpc back
```

Test order: (1) test defaults (HDCP off, 8 bpc). (2) If the link stays up: HDCP on, 8 bpc. (3) HDCP off, 10 bpc.
This shows which of the two causes the drop.

## Debugfs (not in this build)

The prompt asked for DP debugfs. This build does not have it, for these reasons:

- `sm8250-common.config` sets `CONFIG_DEBUG_FS=n`. The DP debugfs nodes need debugfs for the whole kernel.
- A test build with `CONFIG_DEBUG_FS=y` (2026-11-03) did not compile: `techpack/audio/asoc/codecs/wcd-mbhc-v2.c`
  uses `DEBUGFS_DIR_NAME` and `DEBUGFS_HEADSET_STATUS_FILE_NAME`, which are not defined, and
  `techpack/display/msm/dsi/dsi_ctrl.c` has an unused label (`-Werror`).
- Worse, `dsi_ctrl_debugfs_init()` in `dsi_ctrl.c` calls itself (no end) and uses `dir` without a value. With debugfs
  on, the probe of the main panel would very probably overflow the stack at boot. The debugfs code of this tree was
  never built, so other paths can be broken too.
- The `msm-dp-pipa` log lines and the module parameters give the same data (link, mode, bpp, HPD and attention
  events, HDCP on or off) without this risk.

If debugfs is necessary later: fix `dsi_ctrl_debugfs_init()` first (create `dir` with `debugfs_create_dir`, no
self-call, `return 0` before the error labels), then define the two audio names. Test that kernel with
`fastboot boot` only.

## What to look for in dmesg

```sh
adb shell "su -c 'dmesg -w'" | grep -E "msm-dp-pipa|msm-dp-err|hdcp|usbpd|link training"
```

- `hpd event: ... -> attention (IRQ_HPD)` lines: count them after the stream starts. Many in 2 s = the IRQ_HPD loop.
- `attention: no link request, CP_IRQ path (hdcp off)`: with HDCP off, the sink still sends CP_IRQ. Then HDCP is not
  the cause.
- `attention: sink_request=... link_status_updated`: the sink reports a lost link. Then the driver retrains the link.
  Compare `link up:` lines (lanes, rate, levels) before and after.
- `mode 2560x1440@60 pclk=... bpp=24`: the cap works (30 = 10 bpc, 24 = 8 bpc).
- `hpd event: hpd_high=0 ... -> disconnect (HPD low)`: the time of the drop. Look at the lines just before it.
- `HDCP not started (pipa_dp_hdcp=0)`: HDCP is off for this connect. No `key_verify` lines must come.

## Build

```sh
CLANG=/path/to/clang-r547379 ./build-pipa-cos17.sh
```

The Image from this branch has no KPM patch. To get KPM, patch it on your own PC with `kptools-linux` (README). Do not
give the superkey to a cloud session and do not publish an Image that has it. Pack only with `pack-boot.sh` and test
with `fastboot boot` of a trimmed image first (README).

## Roll back

`fastboot boot` changes nothing on the tablet: a reboot goes back to the flashed kernel. If you flashed the test
kernel, flash the last boot image (walt-r13) to `boot_b` again.

## Test image

`out/Image-dptest.gz` on this branch (gzip of `arch/arm64/boot/Image`; the build host could not create a GitHub
release). Built 2026-11-03 with AOSP clang r547379 (clang 20.0.0), default config of `build-pipa-cos17.sh` (KernelSU
on, no debugfs, no `DBG=1`). Build log: 0 warnings.

| File | sha256 |
|---|---|
| `Image` (after `gunzip`) | `0553ae6cc7e2954644b1a2630c7886fa4df000d9c4af16b70fa3c7e168eba1ca` |
| `out/Image-dptest.gz` | `27878cd47e51301ddccdc28474e32dcb3a8041954f8e49c6f05ad1394926da79` |

The Image has no KPM patch. `gunzip -k out/Image-dptest.gz`, patch with KPM on your own PC if you need it, then pack
with `TRIM=1 ./pack-boot.sh` and `fastboot boot` it.
