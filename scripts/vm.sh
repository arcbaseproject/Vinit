#!/bin/sh
# Disposable Void VM for trying vinit on a real distro.
#   scripts/vm.sh install   create the disk (once) and boot the installer ISO
#   scripts/vm.sh           boot the installed system
set -eu
disk=$HOME/vm/void-musl.qcow2
iso=$HOME/Downloads/void-live-x86_64-musl-20250202-base.iso

mkdir -p "$HOME/vm"
[ -f "$disk" ] || qemu-img create -f qcow2 "$disk" 8G

set -- "${1:-}"
cdrom=
[ "$1" = install ] && cdrom="-cdrom $iso -boot d"

# The project directory is shared read-only; inside the VM:
#   mount -t 9p -o trans=virtio,version=9p2000.L vinit /mnt
exec qemu-system-x86_64 -enable-kvm -cpu host -m 2G -smp 2 \
	-drive file="$disk",if=virtio $cdrom \
	-nic user,model=virtio-net-pci \
	-virtfs local,path="$(cd "$(dirname "$0")/.." && pwd)",mount_tag=vinit,security_model=none,readonly=on
