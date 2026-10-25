#!/bin/sh
# Install vinit on Void Linux next to runit. runit stays the default boot
# entry; GRUB gets an extra entry, "Void Linux (vinit)". Run as root from
# a checkout that has ./vinit and ./vinitctl built (static).
#
# Assumes / holds /boot (no separate /boot partition). Rerun after a
# kernel update, so the GRUB entry points at the new kernel.
set -eu
src=$(cd "$(dirname "$0")/../.." && pwd)

install -m755 "$src/vinit" "$src/vinitctl" /usr/bin/
install -Dm755 "$src/contrib/void/sv" /etc/vinit/compat/sv
for c in poweroff reboot halt; do
	install -Dm755 "$src/contrib/void/power" /usr/local/sbin/$c
done
# Void's stage 3 does the distribution shutdown work: hardware clock, RNG
# seed, udev, unmounting, LVM and crypt.
ln -sf /etc/runit/3 /etc/vinit/shutdown
sh "$src/contrib/void/void-import.sh"

kernel=$(ls /boot/vmlinuz-* | sort -V | tail -n 1)
version=${kernel#/boot/vmlinuz-}
uuid=$(findmnt -no UUID /)
cat >/etc/grub.d/42_vinit <<EOF
#!/bin/sh
cat <<'ENTRY'
menuentry 'Void Linux (vinit)' {
	insmod part_gpt
	insmod part_msdos
	insmod ext2
	search --no-floppy --fs-uuid --set=root $uuid
	linux /boot/vmlinuz-$version root=UUID=$uuid ro loglevel=4 init=/usr/bin/vinit
	initrd /boot/initramfs-$version.img
}
ENTRY
EOF
chmod 755 /etc/grub.d/42_vinit
grub-mkconfig -o /boot/grub/grub.cfg

echo
echo "Done. Reboot and pick 'Void Linux (vinit)' in the GRUB menu."
echo "The default entry still boots runit."
