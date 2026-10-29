#!/bin/sh
# Convert the units in tests/units and check the result with vinit's
# own parser. Type=forking must be refused.
set -eu
cd "$(dirname "$0")/.."
out=build/unit2vinit
rm -rf "$out"
mkdir -p "$out"
fail=0

contrib/systemd/unit2vinit -o "$out" tests/units/web.service \
	tests/units/db.service tests/units/setup.service 2>"$out/notes"
if contrib/systemd/unit2vinit tests/units/old.service >/dev/null 2>&1; then
	echo "FAIL Type=forking was converted"
	fail=1
fi
tests/conf-check "$out"/*.conf || fail=1

expect() {
	grep -qxF -- "$2" "$out/$1.conf" || { echo "FAIL $1.conf lacks: $2"; fail=1; }
}
expect web 'command = /bin/sh -c "/usr/bin/mkdir -p /run/web || true && exec /usr/bin/web --port 8080 --root /srv/www"'
expect web 'requires = db'
expect web 'policy = on-failure'
expect web 'delay_ms = 2000'
expect web 'limit = 5'
expect web 'stop_timeout_ms = 90000'
expect web 'working_directory = /srv/www'
expect web 'env = LANG=C.UTF-8'
expect web 'env = MODE=prod'
expect db 'command = /usr/bin/db --data "/var/lib/db"'
expect db 'policy = always'
expect setup 'type = oneshot'
expect setup 'command = /bin/sh -c "/bin/echo one && exec /usr/bin/touch /run/ready-$HOSTNAME"'
grep -q 'Type=notify became simple' "$out/notes" || { echo "FAIL no note for Type=notify"; fail=1; }
expect web 'memory_max = 512M'
expect web 'pids_max = 64'

[ "$fail" = 0 ] && echo "unit2vinit: all checks passed"
exit "$fail"
