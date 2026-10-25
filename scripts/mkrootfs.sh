#!/bin/sh
# Build the test root filesystem: vinit as /sbin/init, the test service
# program as /bin/svc-test, and the services from tests/services. vinit
# creates /proc, /sys, /dev and /run itself.
set -eu
root=$1
rm -rf "$root"
mkdir -p "$root/sbin" "$root/bin" "$root/etc/vinit/services"
cp vinit "$root/sbin/init"
cp vinitctl "$root/sbin/vinitctl"
cp tests/svc-test "$root/bin/svc-test"
cp tests/svc-test "$root/etc/vinit/shutdown"
cp tests/services/*.conf "$root/etc/vinit/services/"
chmod 0644 "$root"/etc/vinit/services/*.conf
chmod 0666 "$root/etc/vinit/services/writable.conf"
cat >"$root/etc/passwd" <<'EOF'
root:x:0:0:root:/:/bin/sh
nobody:x:65534:65534:nobody:/:/bin/false
EOF
cat >"$root/etc/group" <<'EOF'
root:x:0:
nogroup:x:65534:
EOF
