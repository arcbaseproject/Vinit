#!/bin/sh
# Generate vinit services from Void's runit setup:
#   sysinit.conf   runs /etc/runit/1 (stage 1: mounts, fsck, udev, ...) once
#   rc-local.conf  runs /etc/rc.local, if executable, like runit stage 2
#   <name>.conf    one per enabled service in /var/service, run unchanged
# Rerun it after enabling or disabling runit services, then run
# "vinitctl reload".
set -eu
out=/etc/vinit/services
mkdir -p "$out"
chmod 755 /etc/vinit "$out"

write() {
	tmp=$out/.$1.tmp
	cat >"$tmp"
	chmod 644 "$tmp"
	mv "$tmp" "$out/$1.conf"
}

write sysinit <<'EOF'
# Void's runit stage 1: pseudo filesystems, udev, fsck, fstab mounts, swap,
# hostname, sysctl. Everything else requires it; if it fails, vinit starts
# an emergency shell.
[service]
command = /etc/runit/1
type = oneshot
critical = yes

[execution]
stdin = console
EOF

after=sysinit
if [ -x /etc/rc.local ]; then
	write rc-local <<'EOF'
[service]
command = /etc/rc.local
type = oneshot

[dependencies]
requires = sysinit

[execution]
stdin = console
EOF
	after="sysinit rc-local"
fi

for link in /var/service/*; do
	[ -e "$link" ] || continue
	name=${link##*/}
	dir=$(readlink -f "$link")
	case $name in
	[!A-Za-z0-9_]* | *[!A-Za-z0-9_.@-]*)
		echo "skipping $name: name not supported by vinit"
		continue ;;
	esac
	if [ ! -x "$dir/run" ]; then
		echo "skipping $name: no run script"
		continue
	fi
	# "sv check X" in a run script means it needs X up first.
	deps=sysinit
	for d in $(sed -n 's/.*sv check \([A-Za-z0-9_.@-]*\).*/\1/p' "$dir/run"); do
		[ -e "/var/service/$d" ] && deps="$deps $d"
	done
	write "$name" <<EOF
# Generated from $dir by void-import.sh.
[service]
command = $dir/run

[restart]
policy = always
delay_ms = 1000

[dependencies]
requires = $deps
after = $after

[execution]
# Run scripts expect their own directory (agetty reads the tty name from
# it) and runit's PATH; /etc/vinit/compat provides sv(8).
working_directory = $dir
env = PATH=/etc/vinit/compat:/usr/bin:/usr/sbin
EOF
	echo "imported $name"
done
