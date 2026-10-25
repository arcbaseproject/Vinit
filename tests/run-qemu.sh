#!/bin/sh
# Boot the test initramfs in a disposable QEMU VM and check the serial log.
# KERNEL overrides the kernel image (default: the running host kernel).
set -u
initrd=${1:-build/initramfs.cpio.gz}
kernel=${KERNEL:-/boot/vmlinuz-$(uname -r)}
log=build/qemu.log

accel=
[ -w /dev/kvm ] && accel="-enable-kvm -cpu host"

# panic=-1 plus -no-reboot turns a PID 1 crash into a QEMU exit, not a hang.
boot() {
	timeout 120 qemu-system-x86_64 $accel -m 128M -nographic -no-reboot -audio none \
		-kernel "$kernel" -initrd "$initrd" \
		-append "console=ttyS0 rdinit=/sbin/init panic=-1 loglevel=4 $1" \
		</dev/null >"$2" 2>&1
}

boot "" "$log"
status=$?
cat "$log"

fail=0
. tests/checks.sh
run_checks "$log"

# In a real VM the device filesystems must mount.
if grep -q 'vinit: mount' "$log"; then
	echo "FAIL: a mount failed"
	fail=1
fi
if grep -q 'cannot unmount' "$log"; then
	echo "FAIL: a filesystem stayed writable at shutdown"
	fail=1
fi
if grep -q 'Kernel panic' "$log"; then
	echo "FAIL: kernel panic"
	fail=1
fi
[ "$status" -eq 0 ] || { echo "FAIL: qemu exit status $status"; fail=1; }

# vinit.quiet hides routine messages but keeps warnings.
qlog=build/qemu-quiet.log
boot vinit.quiet "$qlog"
if grep -qF 'vinit: booting' "$qlog" || grep -qF 'vinit: started base' "$qlog"; then
	echo "FAIL: vinit.quiet printed routine messages"
	fail=1
fi
grep -qF "bad.conf:3: unknown key 'colour'" "$qlog" || { echo "FAIL: vinit.quiet hid a warning"; fail=1; }
grep -qF 'svc-test check: PASS' "$qlog" || { echo "FAIL: quiet boot did not pass"; fail=1; }

[ "$fail" -eq 0 ] && echo "qemu test: PASS"
exit "$fail"
