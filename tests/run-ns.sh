#!/bin/sh
# Run vinit as PID 1 inside unprivileged user, PID, mount and network
# namespaces, chrooted into the test root filesystem. This exercises real
# PID 1 behavior (orphan reaping, kill(-1), reboot()) without root and
# without touching the host's init.
#
# Inner uids 1-65535 map to the caller's /etc/subuid range (newuidmap),
# so services can switch to uid 65534.
set -u
log=build/ns.log

# The tests change files in the root filesystem (reload), so boot a copy.
rm -rf build/ns-root
cp -a "${1:-build/rootfs}" build/ns-root
root=build/ns-root

timeout 60 unshare --user --map-root-user \
	--map-users=1:"$(awk -F: -v u="$(id -un)" '$1 == u { print $2 + 1 }' /etc/subuid)":65535 \
	--map-groups=1:"$(awk -F: -v u="$(id -un)" '$1 == u { print $2 + 1 }' /etc/subgid)":65535 \
	--pid --fork --mount --net --root="$root" /sbin/init >"$log" 2>&1
status=$?
cat "$log"

fail=0
. tests/checks.sh
run_checks "$log"

# reboot(RB_POWER_OFF) in a PID namespace kills the namespace init with
# SIGINT; unshare re-raises it, so the shell sees 128 + 2.
[ "$status" -eq 130 ] || { echo "FAIL: exit status $status, want 130"; fail=1; }

if ./vinit 2>/dev/null; then
	echo "FAIL: vinit ran outside PID 1"
	fail=1
fi

[ "$fail" -eq 0 ] && echo "ns test: PASS"
exit "$fail"
