#!/bin/sh
# Install vinit on Alpine Linux next to busybox init. Run as root from a
# checkout that has ./vinit and ./vinitctl built (static).
#
# This script does not change the boot loader. To boot vinit, add
#   init=/usr/bin/vinit
# to the kernel command line for one boot (for example, press Tab at the
# extlinux prompt, or "e" in GRUB). Without it, busybox init boots as
# before.
set -eu
src=$(cd "$(dirname "$0")/../.." && pwd)

install -m755 "$src/vinit" "$src/vinitctl" /usr/bin/
mkdir -p /etc/vinit
# "openrc shutdown" stops OpenRC's daemons and unmounts what it mounted,
# as the ::shutdown: line in /etc/inittab does under busybox init.
cat >/etc/vinit/shutdown <<'EOF2'
#!/bin/sh
exec /sbin/openrc shutdown
EOF2
chmod 755 /etc/vinit/shutdown
sh "$src/contrib/alpine/alpine-import.sh"

echo
echo "Done. Boot once with init=/usr/bin/vinit on the kernel command line."
echo "busybox poweroff, reboot and halt signal PID 1 the way vinit expects,"
echo "so they work unchanged."
