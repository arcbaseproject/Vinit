#!/bin/sh
# Generate vinit services from Alpine's busybox init setup:
#   openrc-sysinit.conf  runs "openrc sysinit" once (critical)
#   openrc-boot.conf     runs "openrc boot" once
#   openrc-default.conf  runs "openrc default" once
#   getty-<tty>.conf     one per "respawn" line in /etc/inittab
# OpenRC keeps starting and stopping its own daemons, so "rc-service"
# and "rc-update" work as before. vinit supervises only the gettys.
# Rerun it after you change /etc/inittab, then run "vinitctl reload".
set -eu
# ROOT lets the tests run this on a copy of /etc.
out=${ROOT:-}/etc/vinit/services
mkdir -p "$out"
chmod 755 "${ROOT:-}/etc/vinit" "$out"

write() {
	tmp=$out/.$1.tmp
	cat >"$tmp"
	chmod 644 "$tmp"
	mv "$tmp" "$out/$1.conf"
}

write openrc-sysinit <<'EOF2'
# OpenRC sysinit runlevel: devfs, dmesg, mdev or udev. Everything else
# requires it; if it fails, vinit starts an emergency shell.
[service]
command = /sbin/openrc sysinit
type = oneshot
critical = yes

[execution]
stdin = console
EOF2

write openrc-boot <<'EOF2'
# OpenRC boot runlevel: fsck, mounts, hostname, sysctl, networking.
[service]
command = /sbin/openrc boot
type = oneshot

[dependencies]
requires = openrc-sysinit

[execution]
stdin = console
EOF2

write openrc-default <<'EOF2'
# OpenRC default runlevel. The daemons it starts detach, and OpenRC
# tracks them. vinit kills what is left at shutdown, after "openrc
# shutdown" has run.
[service]
command = /sbin/openrc default
type = oneshot
stop_timeout_ms = 30000

[dependencies]
requires = openrc-boot

[execution]
stdin = console
EOF2

# Remove gettys from an earlier run, so that a removed inittab line
# removes its service too.
rm -f "$out"/getty-*.conf

# Lines look like "tty1::respawn:/sbin/getty 38400 tty1".
grep -v '^[[:space:]]*#' "${ROOT:-}/etc/inittab" | while IFS=: read -r id _ action cmd; do
	[ "$action" = respawn ] || continue
	case $id in
	'' | [!A-Za-z0-9_]* | *[!A-Za-z0-9_.@-]*)
		echo "skipping inittab line for '$id': id not usable as a service name"
		continue ;;
	esac
	case $cmd in
	/*) ;;
	*)
		echo "skipping $id: command is not an absolute path"
		continue ;;
	esac
	write "getty-$id" <<EOF2
# Generated from /etc/inittab by alpine-import.sh.
[service]
command = $cmd

[restart]
policy = always
delay_ms = 1000

[dependencies]
requires = openrc-default
EOF2
	echo "imported getty-$id"
done
