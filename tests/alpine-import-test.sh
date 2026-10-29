#!/bin/sh
# Run contrib/alpine/alpine-import.sh on a sample /etc/inittab and check
# the result with vinit's own parser.
set -eu
cd "$(dirname "$0")/.."
root=build/alpine-root
rm -rf "$root"
mkdir -p "$root/etc"
cat >"$root/etc/inittab" <<'INITTAB'
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
tty1::respawn:/sbin/getty 38400 tty1
tty2::respawn:/sbin/getty 38400 tty2
#ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/openrc shutdown
INITTAB
ROOT=$root sh contrib/alpine/alpine-import.sh
fail=0
tests/conf-check "$root"/etc/vinit/services/*.conf || fail=1
for s in openrc-sysinit openrc-boot openrc-default getty-tty1 getty-tty2; do
	[ -f "$root/etc/vinit/services/$s.conf" ] || { echo "FAIL missing $s.conf"; fail=1; }
done
[ ! -e "$root/etc/vinit/services/getty-ttyS0.conf" ] || { echo "FAIL commented line imported"; fail=1; }
[ "$fail" = 0 ] && echo "alpine-import: all checks passed"
exit "$fail"
