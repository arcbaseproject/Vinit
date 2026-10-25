#!/bin/sh
# Pack a root filesystem tree into a gzip-compressed newc initramfs.
set -eu
root=$1
out=$2
(cd "$root" && find . | cpio -o -H newc -R 0:0 --quiet) | gzip -9 >"$out"
