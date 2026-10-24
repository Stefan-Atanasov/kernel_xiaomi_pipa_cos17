#!/bin/bash
# Build the pipa ColorOS 17 kernel Image. Usage: CLANG=/path/to/clang-r547379 ./build-pipa-cos17.sh
# Env: OUT (default out), DBG=1 (boot watchdog + lockup panics), CLEAN=0 (keep out/), KCONFIG_EXTRA (scripts/config args)
set -euo pipefail
cd "$(dirname "$0")"
CLANG=${CLANG:?set CLANG to an AOSP clang (r547379 = clang 20)}
export PATH="$CLANG/bin:$PATH"
OUT=${OUT:-out}
A=(ARCH=arm64 O="$OUT" CC=clang LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_COMPAT=arm-linux-gnueabi-
   AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip)
[ "${CLEAN:-1}" = 1 ] && rm -rf "$OUT"
mkdir -p "$OUT"
C=arch/arm64/configs/vendor
F=("$C/kona-perf_defconfig" "$C/xiaomi/sm8250-common.config" "$C/xiaomi/pipa.config" "$C/xiaomi/pipa-cos17.config")
[ -f "$C/xiaomi/pipa-oplus.config" ] && F+=("$C/xiaomi/pipa-oplus.config")
ARCH=arm64 LLVM=1 scripts/kconfig/merge_config.sh -O "$OUT" "${F[@]}" > "$OUT/merge_config.log" 2>&1 ||
	{ tail -20 "$OUT/merge_config.log"; exit 1; }
X=()
[ -d KernelSU ] && X+=(-e KSU -e KSU_SUSFS -e KSU_SUSFS_SUS_PATH -e KSU_SUSFS_SUS_MOUNT -e KSU_SUSFS_SUS_KSTAT
	-e KSU_SUSFS_SPOOF_UNAME -e KSU_SUSFS_ENABLE_LOG -e KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS
	-e KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG -e KSU_SUSFS_OPEN_REDIRECT -e KSU_SUSFS_SUS_MAP)
[ "${DBG:-0}" = 1 ] && X+=(-e PIPA_BOOTDBG -e SOFTLOCKUP_DETECTOR -e BOOTPARAM_SOFTLOCKUP_PANIC -e DETECT_HUNG_TASK
	-e BOOTPARAM_HUNG_TASK_PANIC --set-val DEFAULT_HUNG_TASK_TIMEOUT 60 -e PANIC_ON_OOPS)
if [ ${#X[@]} -gt 0 ] || [ -n "${KCONFIG_EXTRA:-}" ]; then scripts/config --file "$OUT/.config" "${X[@]}" ${KCONFIG_EXTRA:-}; fi
make "${A[@]}" olddefconfig
make "${A[@]}" -j"$(nproc)"
ls -la "$OUT/arch/arm64/boot/Image"
