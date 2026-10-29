#!/bin/sh
# Cross-build into a separate directory; boot the existing integration suite.
set -eu
: "${ARM64_CC:?set ARM64_CC to an AArch64 Linux C compiler}"
: "${ARM64_KERNEL:?set ARM64_KERNEL to an uncompressed ARM64 Linux Image}"
out=build/arm64
mkdir -p "$out/tests"
"$ARM64_CC" -std=c17 -Wall -Wextra -Wshadow -Os -static \
	-D_FORTIFY_SOURCE=2 -fstack-protector-strong \
	-o "$out/vinit" src/vinit.c src/service.c src/config.c src/control.c
"$ARM64_CC" -std=c17 -Wall -Wextra -Wshadow -Os -static \
	-D_FORTIFY_SOURCE=2 -fstack-protector-strong \
	-o "$out/vinitctl" cmd/vinitctl.c
"$ARM64_CC" -std=c17 -Wall -Wextra -Wshadow -Os -static \
	-o "$out/tests/svc-test" tests/svc-test.c
scripts/mkrootfs.sh "$out/rootfs" "$out"
scripts/mkinitramfs.sh "$out/rootfs" "$out/initramfs.cpio.gz"
QEMU_ARCH=aarch64 QEMU_LOG_DIR="$out" KERNEL="$ARM64_KERNEL" \
	sh tests/run-qemu.sh "$out/initramfs.cpio.gz"
