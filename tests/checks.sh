# Assertions on a vinit boot log, shared by run-ns.sh and run-qemu.sh.
# Usage: . tests/checks.sh; run_checks LOG   (sets $fail to 1 on failure)

check() {
	grep -qF -- "$1" "$log" || { echo "FAIL: missing '$1'"; fail=1; }
}

# $1 must appear on an earlier line than $2.
before() {
	a=$(grep -nF -- "$1" "$log" | head -1 | cut -d: -f1)
	b=$(grep -nF -- "$2" "$log" | head -1 | cut -d: -f1)
	if [ -z "$a" ] || [ -z "$b" ] || [ "$a" -ge "$b" ]; then
		echo "FAIL: '$1' not before '$2'"
		fail=1
	fi
}

run_checks() {
	log=$1
	check 'vinit: booting'

	# restart policy and delay
	check 'flaky exited with status 3'
	check 'restarting flaky in 1000 ms'
	check 'svc-test flaky: ok'

	# requires/after ordering: app starts only after setup finished
	check 'finished setup'
	check 'svc-test app: dependencies ready'
	before 'finished setup' 'started app'

	# crash reports, backoff and restart limit
	check 'restarting crashloop in 100 ms'
	check 'restarting crashloop in 200 ms'
	check 'crashloop failed: restart limit (2) reached'
	n=$(grep -cF 'crashloop killed by signal 6' "$log")
	[ "$n" -eq 3 ] || { echo "FAIL: crashloop ran $n times, want 3"; fail=1; }

	# invalid configuration is rejected, and the rest still boots
	check 'service cycle-a failed: dependency cycle: cycle-a -> cycle-b -> cycle-a'
	check 'service cycle-b failed: dependency cycle: cycle-a -> cycle-b -> cycle-a'
	check 'needs-cycle failed: dependency cycle-a is not running'
	check "service missing failed: requires 'nonexistent' is not a known service"
	check "bad.conf:3: unknown key 'colour' in [service]"
	check 'shell syntax is not supported'
	check 'writable.conf: must be owned by uid 0 and not writable by group or others'
	check 'noexec: exec /bin/does-not-exist: No such file or directory'
	check 'noexec failed'

	# privilege dropping, working directory, environment
	check 'svc-test id: uid=65534 gid=65534 groups=1'
	check 'svc-test id: cwd=/run GREETING=hello world'

	# orphan reaping
	check 'svc-test check: zombies after orphans exit: 0'
	check 'svc-test check: PASS'

	# no file descriptor leaks in PID 1 after all the spawns above
	check 'svc-test check: PID 1 open fds: 5'
	check 'svc-test check: PID 1 CPU ticks during 3 s idle: 0 '
	check 'svc-test check: PID 1 open fds after control tests: 5'

	# control socket
	check 'svc-test check: vinitctl stop base: ok'
	check 'stop base requested by uid 0'
	check 'svc-test check: app is running'
	check 'stopped flaky, starting it again'
	check 'svc-test check: raw request -> ok: ok'
	check 'poweroff requested by uid 0'

	# a zero restart delay still backs off; the limit stops it
	check 'restarting spin in 0 ms'
	check 'restarting spin in 200 ms'
	check 'restarting spin in 400 ms'
	check 'spin failed: restart limit (3) reached'

	# stress: process group cleanup, signals, concurrency, restart churn
	check 'svc-test check: leaky child'
	check 'svc-test check: 50 concurrent clients: ok'
	check 'svc-test check: after 100 restarts: fds 5 -> 5'

	# readiness notification
	check 'ready-slow is ready'
	before 'ready-slow is ready' 'started after-ready'
	check 'svc-test after-ready: dependency was ready'
	check 'never-ready not ready after 300 ms'
	check 'never-ready failed'

	# reload
	check 'reloaded: 1 added, 0 removed'
	check 'svc-test check: late is running'
	check 'svc-test check: missing is running'

	# a failed critical service brings up the emergency shell
	check 'critical service critical-fail failed, starting emergency shell'
	check 'emergency-shell: exec /bin/sh'

	# shutdown hook: after the services, before the final kill, in PID 1's session
	check 'svc-test hook: called with poweroff'
	check 'svc-test hook: same session as PID 1: yes'
	before 'stopped base' 'svc-test hook: called with poweroff'
	before 'svc-test hook: called with poweroff' 'sending SIGTERM to all processes'
	check 'unmounting filesystems'

	# shutdown: reverse dependency order, stop timeout, final kill
	check 'shutdown requested, stopping services'
	check 'svc-test app: got SIGTERM'
	check 'svc-test base: got SIGTERM'
	before 'svc-test app: exiting' 'svc-test base: got SIGTERM'
	check 'stubborn did not stop in 500 ms, sending SIGKILL'
	check 'sending SIGKILL to remaining processes'
	check 'powering off'

	if grep -q 'svc-test .*: FAIL' "$log"; then
		grep 'svc-test .*: FAIL' "$log"
		fail=1
	fi
}
