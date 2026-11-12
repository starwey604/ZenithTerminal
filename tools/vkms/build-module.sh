#!/usr/bin/env bash
# Build only. Never unload, install, or load a kernel module.
set -euo pipefail
if (( $# < 2 || $# > 3 )); then
    echo "Usage: $0 VKMS_SOURCE_DIR NEW_OUTPUT_DIR [KERNEL_BUILD_DIR]" >&2
    echo "Source: unmodified drivers/gpu/drm/vkms from the matching kernel tree." >&2
    exit 2
fi
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
source_dir=$(realpath -- "$1")
output=$(realpath -m -- "$2")
kbuild=$(realpath -- "${3:-/lib/modules/$(uname -r)/build}")
[[ -f "$source_dir/vkms_formats.c" && -f "$source_dir/Makefile" ]]
[[ -f "$kbuild/Module.symvers" && -f "$kbuild/.config" ]]
# Do not overwrite a previous candidate or accidentally patch a source tree.
mkdir -- "$output"
cp -- "$source_dir/"*.c "$source_dir/"*.h "$source_dir/Makefile" "$output/"
patch --batch --fuzz=0 -d "$output" -p5 < "$repo/patches/linux-vkms/0001-vkms-optimize-yuv-fixed-point-math.patch"
compiler=()
if grep -qx 'CONFIG_CC_IS_CLANG=y' "$kbuild/.config"; then compiler=(LLVM=1); fi
make -C "$kbuild" M="$output" "${compiler[@]}" CONFIG_DRM_VKMS=m CONFIG_DRM_VKMS_KUNIT_TEST=n -j"${JOBS:-2}" modules
modinfo "$output/vkms.ko"
printf '\nBuilt only: %s/vkms.ko\nSystem module unchanged.\n' "$output"
