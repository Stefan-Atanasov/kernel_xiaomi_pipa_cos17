#!/bin/bash
# Swap a kernel Image into an existing pipa boot.img (header v3, keeps ramdisk and cmdline).
#   ./pack-boot.sh <Image> <base boot.img> <out.img> ["extra cmdline"]     needs magiskboot in PATH
# magiskboot repack rewrites os_version/os_patch_level; a wrong patch level makes the bootloader refuse the kernel
# silently (black screen), so the original 4 bytes at offset 16 are copied back. TRIM=1: header + kernel + ramdisk
# only, for `fastboot boot` (the full 196 MB image hangs it).
set -euo pipefail
[ $# -ge 3 ] || { sed -n 2,6p "$0"; exit 1; }
img=$(realpath "$1"); base=$(realpath "$2"); out=$(realpath -m "$3"); cmd=${4:-}
w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
cd "$w"
magiskboot unpack -h "$base" >/dev/null 2>&1
cp "$img" kernel
[ -n "$cmd" ] && sed -i "s|^cmdline=.*|cmdline=$cmd|" header
magiskboot repack "$base" "$out" >/dev/null 2>&1
dd if="$base" of="$out" bs=1 skip=16 seek=16 count=4 conv=notrunc status=none
cmp -s <(head -c 20 "$base" | tail -c 4) <(head -c 20 "$out" | tail -c 4) || { echo "os_version mismatch" >&2; exit 1; }
if [ "${TRIM:-0}" = 1 ]; then
	r4k() { echo $(( ($1 + 4095) / 4096 * 4096 )); }
	ks=$(od -An -tu4 -j8 -N4 "$out" | tr -d ' '); rs=$(od -An -tu4 -j12 -N4 "$out" | tr -d ' ')
	truncate -s $(( 4096 + $(r4k "$ks") + $(r4k "$rs") )) "$out"
fi
echo "md5 $(md5sum < "$out" | cut -c1-32)  $(stat -c %s "$out") bytes  $out"
