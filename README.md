# vinit

[![CI](https://github.com/arcbaseproject/Vinit/actions/workflows/ci.yml/badge.svg)](https://github.com/arcbaseproject/Vinit/actions/workflows/ci.yml)

A small init (PID 1) and service supervisor for Linux, written in C17. It
does not depend on systemd or any other init system.

vinit can do the following:

- mount the API filesystems
- supervise services with restart policies, dependencies and readiness
  notification
- reap all children
- shut down in reverse dependency order
- take commands from `vinitctl` over a Unix socket, including a safe
  configuration reload

It is about 2000 lines of C. A static musl build of `vinit` is 101 kB.

> **Status: experimental.** vinit passes the tests below on glibc and
> musl, in namespaces and in QEMU, and it boots Void Linux in a VM. It has
> had only a self-review for security ([docs/security.md](docs/security.md)),
> not an independent one. Do not use it in production until it has had
> independent review and use on real hardware.

See [docs/architecture.md](docs/architecture.md) for the design.

**Contents:** [Build](#build) · [Services](#services) ·
[vinitctl](#vinitctl) · [Logging](#logging) ·
[Void Linux](#use-on-void-linux) · [Test](#test) ·
[Measurements](#measurements) · [Known limitations](#known-limitations) ·
[License](#license)

## Build

Nix/NixOS users: see [nix/README.md](nix/README.md) for the package and
NixOS service module, which runs Vinit alongside systemd.

```sh
make                       # static ./vinit and ./vinitctl
make clean all CC=musl-gcc # musl build (clean first: make does not track CC)
make LDFLAGS=              # dynamic build
```

The build adds `-D_FORTIFY_SOURCE=2 -fstack-protector-strong`. musl
ignores the fortify define.

## Services

vinit reads every `*.conf` file in `/etc/vinit/services` at boot and
starts all of them. To disable a service, rename or remove its file.

A file named `sshd.conf` defines the service `sshd`:

```ini
[service]
name = sshd                     # optional; must match the file name
command = /usr/sbin/sshd -D     # required
type = simple                   # simple (default) | oneshot
stop_timeout_ms = 5000          # SIGTERM, then SIGKILL after this
ready_fd = 3                    # optional readiness notification, see below
ready_timeout_ms = 10000        # fail if not ready by then; 0 (default) = wait
critical = no                   # yes: failure starts the emergency shell

[restart]
policy = on-failure             # no (default) | on-failure | always
delay_ms = 1000                 # first restart delay; doubles each time
limit = 5                       # give up after N restarts in a row; 0 = never

[dependencies]
requires = network              # start network first; fail if it fails
after = syslog                  # only ordering: wait if syslog is starting

[execution]
user = sshd                     # name or number from /etc/passwd
group = sshd                    # overrides the user's primary group
working_directory = /
env = LANG=C.UTF-8              # may be repeated
stdin = null                    # null (default) | console
log = sshd-log                  # optional: send stdout and stderr to this service

[limits]                        # all optional; each needs cgroup v2
memory_max = 512M               # bytes, or K/M/G/T (powers of 1024)
pids_max = 64                   # processes and threads
cpu_max = 50%                   # of one CPU; 200% = two CPUs
```

The `#` comments above are for explanation only. In a real file, comments
must be on their own line and start with `#` or `;`.

### Rules

**The parser is strict.** vinit rejects the whole file in these cases:

- an unknown section or key
- a duplicate key
- an empty or malformed value
- a line over 1023 bytes

It also rejects a file that is not owned by root or that is writable by
group or others. Each error is logged with the file name and line number.
A rejected service is marked failed, and the other services still start.

**Commands never run through a shell.** vinit splits a command into words
at whitespace. Double quotes group words, and `\"` and `\\` are the only
escapes inside quotes. The characters `` ' ` $ | ; & < > \ `` outside
quotes are rejected. If a service needs a shell, write
`command = /bin/sh -c "..."`. The first word must be an absolute path.

**`requires` dependencies:**
- The service starts only when the dependency is ready. A `simple`
  dependency is ready once it has been executed. A `oneshot` dependency is
  ready once it has exited with status 0.
- If the dependency fails, the service fails too.
- If the dependency exits later on its own, vinit stops the service too.
  The service starts again when the dependency is ready again.
- `requires` also implies ordering.

**Readiness.** Without `ready_fd`, a `simple` service counts as ready as
soon as it has been executed. With `ready_fd = N`, vinit gives the service
the write end of a pipe as fd N. The service stays `starting` until it
writes a newline there. This is the s6 protocol, and s6-style daemons
already support it. vinit closes its end after the newline, so the
service should close fd N too. If `ready_timeout_ms` passes first, vinit
stops the service and counts the run as a failure, so the restart policy
applies. `ready_fd` works only with `type = simple`.

**`after` dependencies** only affect ordering. The service waits while the
other service is starting or waiting to restart. It does not wait for a
service that has failed or stopped.

vinit rejects dependency cycles at load time, counting both kinds of
dependency, and logs the cycle path. It also rejects names of services that
do not exist.

**Restarts:**
- `policy = always` also repeats successful oneshots, with the same backoff
  and restart limit. Between runs they are not ready for dependents.
- The delay doubles after each consecutive restart, up to 60 s.
- A run that lasts 10 s or more resets the restart count.
- When the process of a `simple` service exits, vinit kills the rest of
  its process group and cgroup.
- A `oneshot` may leave background processes running on purpose.

**cgroups.** vinit mounts cgroup v2 on `/sys/fs/cgroup` if it is not
mounted yet. It moves itself into `init.scope` in the cgroup it started
in, and runs each service in `<name>.service` next to it. Stop and kill
signals go to the process group and to every process in the cgroup, so
processes that call `setsid()` or `setpgid()` cannot escape. Without
cgroup v2, for example in a user namespace, services run in process
groups only, and a service with `[limits]` fails to start.

**Shutdown** stops services in reverse dependency order. A service stops
only after every service that depends on it has exited. vinit sends
SIGTERM to the service's whole process group and cgroup, then SIGKILL after
`stop_timeout_ms`. Then vinit does these steps:

1. If `/etc/vinit/shutdown` is executable, run it with the argument
   `reboot`, `poweroff` or `halt`. After 120 s, kill it.
2. Send SIGTERM to all remaining processes, then SIGKILL.
3. Turn off swap.
4. Unmount every filesystem except `/`, children first. If a filesystem
   is busy, remount it read-only.
5. Remount `/` read-only, then reboot, power off or halt.

**Emergency shell.** vinit starts `/bin/sh` on the console in these cases:

- No service file is usable.
- A service with `critical = yes` fails.

## vinitctl

```sh
vinitctl status            # summary: counts per state
vinitctl status sshd       # state, pid, type, restarts, last exit, error
vinitctl list              # one line per service
vinitctl start sshd        # also starts what sshd requires
vinitctl stop sshd         # also stops what requires sshd, first
vinitctl restart sshd      # services that depend on sshd keep running
vinitctl reload            # re-read /etc/vinit/services
vinitctl reboot | poweroff | halt
```

Any user may run `status` and `list`. Every other command needs uid 0.
The `start`, `stop` and `restart` commands return as soon as vinit has
accepted them. To follow progress, run `vinitctl status`.

`reload` re-reads every service file. It is all or nothing: vinit refuses
the reload, and changes nothing, in these cases:

- It would put a service that works now into an error state. Examples are
  a typo, or a new dependency cycle.
- It would remove a service that is still active. Stop the service first.

When the reload goes through, new services start and services that had a
config error and are now fixed start too. Running services whose
definition changed restart, as with `vinitctl restart`: their dependents
keep running. Unchanged services keep running.

The exit status is 0 on success, 1 when vinit reports an error, and 2 for
usage or connection errors.

## Logging

vinit writes `vinit: ...` lines to the console. Add `vinit.quiet` to the
kernel command line to show only warnings and errors: failures, crashes,
config errors and timeouts. Services write to the console directly,
unless they set `log`. If the console is stalled for more than 1 s, vinit
drops its own log lines until the console accepts writes again. Services
still block.

`log = X` sends the service's stdout and stderr through a pipe to the
stdin of service `X`, as runit's log services do. A logger such as
`svlogd` handles files and rotation:

```ini
# /etc/vinit/services/sshd-log.conf
[service]
command = /usr/bin/svlogd -tt /var/log/sshd
```

vinit holds both ends of the pipe, so no output is lost when either side
restarts. `log` also implies `after = X`. Several services may log to the
same `X`. If `X` stops reading, the pipe fills (64 KiB on Linux), and
then the services that log to it block. `X` must not use
`stdin = console`.

Services get only `PATH`, `TERM` and their own `env` lines in their
environment. vinit drops other variables, such as `KEY=value` words from
the kernel command line.

### Signals to PID 1

| Signal                       | Action    |
|------------------------------|-----------|
| SIGTERM, SIGINT (Ctrl-Alt-Del) | reboot  |
| SIGUSR1                      | halt      |
| SIGUSR2                      | power off |

## Use on Void Linux

[contrib/void](contrib/void) installs vinit next to runit. runit stays
the default boot entry, so you can always go back. Try it in a VM first:
[scripts/vm.sh](scripts/vm.sh) creates and boots a disposable Void VM.

As root, from a checkout with static `./vinit` and `./vinitctl` built:

```sh
sh contrib/void/install.sh
```

The script does these steps:

- It installs `vinit` and `vinitctl` in `/usr/bin`.
- It makes Void's runit stage 1 (`/etc/runit/1`) a critical oneshot
  service named `sysinit`.
- It makes each enabled service in `/var/service` a vinit service.
  `sv check X` in a run script becomes a dependency on `X`.
- It makes runit stage 3 the shutdown hook, `/etc/vinit/shutdown`.
- It replaces `poweroff`, `reboot` and `halt` in `/usr/local/sbin` with
  wrappers. Under vinit they call `vinitctl`. Under runit they run Void's
  own commands.
- It adds the GRUB entry "Void Linux (vinit)".

Reboot and pick that entry. After you enable or disable a runit service,
run `sh contrib/void/void-import.sh` and then `vinitctl reload`. After a
kernel update, run `install.sh` again, so that the GRUB entry points at
the new kernel. The script assumes that `/boot` is on the root
filesystem.

## Test

Never install vinit as the init of your development machine. The tests
below run it in a disposable environment.

```sh
make test-unit   # parser, supervision and event/control checks, under ASan and UBSan
make test        # test-unit, then vinit as PID 1 in namespaces (no root)
make test-qemu   # boot the same root filesystem in QEMU
make fuzz        # libFuzzer on parser + resolver (clang); FUZZ_TIME=60 s
```

To test the musl build, run:

```sh
make clean
make CC=musl-gcc test test-qemu
```

`make test` needs the following:

- unprivileged user namespaces
- util-linux `unshare`
- an entry for your user in `/etc/subuid` and `/etc/subgid`, so that a
  service can switch to uid 65534

It does not work inside a Flatpak sandbox. `make test-qemu` uses the host
kernel by default. To use another kernel, set `KERNEL=/path/to/bzImage`.

The same boot and shutdown checks also run under ARM64 QEMU emulation.
Install `qemu-system-aarch64`, supply an AArch64 Linux compiler with static
libc and an uncompressed ARM64 kernel `Image`, then run:

```sh
make test-qemu-arm64 \
  ARM64_CC=/path/to/aarch64-linux-musl-gcc \
  ARM64_KERNEL=/path/to/Image
```

This builds into `build/arm64` without replacing the host-architecture
binaries. Logs are `build/arm64/qemu.log` and `qemu-quiet.log`.

Both integration tests boot `build/rootfs`. In it, the services from
[tests/services](tests/services) run [tests/svc-test.c](tests/svc-test.c),
and [tests/checks.sh](tests/checks.sh) checks the boot log:

| Area | What the test checks |
|---|---|
| Restarts | Restart after exit 3 waits 1000 ms. A crash loop restarts after 100 ms, then 200 ms, then stops at `limit`. |
| Dependencies | A `requires` on a oneshot waits until the oneshot finishes. |
| Bad config | The log reports cycles, unknown dependencies, unknown keys, shell syntax, unsafe permissions, and exec failures. Boot continues. |
| Privileges | The service runs as uid 65534, with its working directory and environment. |
| PID 1 hygiene | 20 orphans are reaped. PID 1 has exactly 5 fds open (stdio, signalfd, socket), also after a client passes it fds. |
| Idle CPU | PID 1 uses 0 CPU ticks during 3 s of idle. |
| Control: commands | `list` and `status` work. `stop base` stops `app` and `setup` first. `start app` brings `base` and `setup` back. `restart` works. |
| Control: rejections | Unknown commands and services, usage errors, config errors, non-root changes, malformed and oversized datagrams are all rejected. |
| Readiness | A dependent of a `ready_fd` service waits until the service writes its newline. A service that never becomes ready fails after `ready_timeout_ms`. |
| Reload | A new service starts. A broken edit to `base.conf` is refused. Removing a running service is refused until it is stopped. A fixed service starts. |
| Backoff | A service that exits at once, with `delay_ms = 0`, restarts after 0, 200, then 400 ms, then stops at `limit`. |
| Process groups | A child left behind by a simple service that exited is killed. |
| Robustness | PID 1 survives every catchable signal (1-31 without an action, plus 4 realtime signals). 50 concurrent `vinitctl` clients all get answers. |
| Leaks | After 100 restarts of a service, PID 1 has the same fds and the same RSS. |
| Quiet | With `vinit.quiet` (QEMU only), no routine lines are printed, warnings still are, and the test still passes. |
| Shutdown | A dependent exits before its dependency gets SIGTERM. The stop timeout leads to SIGKILL. A detached orphan gets the final SIGKILL. |

## Measurements

These were measured on 2026-10-24 on x86_64 with gcc 15.2, `-Os`, static
linking, and host kernel 6.18.58 in QEMU with KVM and 128 MB of RAM. The
tests print the runtime numbers, so `make test` or `make test-qemu`
reproduces them.

| What                                  | glibc 2.41            | musl 1.2.5          |
|---------------------------------------|-----------------------|---------------------|
| `vinit` (stripped)                    | 3,589,408 B (749,984) | 101,224 B (79,288)  |
| `vinitctl` (stripped)                 | 3,224,232 B (692,624) | 37,600 B (30,384)   |
| PID 1 RSS: 23 services, 4 reloads, 100 restarts | 960 kB      | 288 kB              |
| PID 1 CPU while idle                  | 0 ticks in 3 s        | 0 ticks in 3 s      |
| Kernel start to first service         | 0.29 s                | 0.25 s              |

The source in `src/` and `cmd/` is 2011 lines, and the tests are 1056
lines. A 5-minute fuzzing run (`make fuzz FUZZ_TIME=300`) did 3,274,552
executions under ASan and UBSan, with no crash and no contract violation.

## Known limitations

- `type = forking` (daemons that background themselves) is not supported.
  Run daemons in the foreground.
- `[limits]` covers memory, process count and CPU only. For other limits,
  such as open files, wrap the command with `prlimit` (util-linux) or
  `chpst -o` (runit).
- The console is never a controlling terminal, so shells started on it
  have no job control. Use a getty on a real tty for that.
- x86_64 and ARM64 have passed QEMU boot tests; real hardware remains untested.
- See [docs/security.md](docs/security.md) for security gaps.

## License

MIT. See [LICENSE](LICENSE).
