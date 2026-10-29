# vinit architecture

## Source layout

| File            | Role                                                  |
|-----------------|-------------------------------------------------------|
| `src/vinit.c`   | PID 1: boot, mounts, main loop, final shutdown, logging |
| `src/service.c` | Supervision: start, stop, restart, reaping, privilege drop |
| `src/config.c`  | Service file parser, dependency resolution, cycle check |
| `src/control.c` | Control socket: requests, credentials, replies        |
| `src/vinit.h`   | Shared types and limits                               |
| `cmd/vinitctl.c`| Control client                                        |

`config.c` has no runtime dependencies, so `tests/config-test.c` links it
alone.

## Process model

vinit is one single-threaded process, PID 1. Each service runs in its own
session (`setsid()`), so its PID is also its process group ID. Stopping a
service signals the whole group.

With cgroup v2, each service also runs in its own cgroup,
`<name>.service`, below the cgroup that PID 1 started in. PID 1 moves into
`init.scope` there first, because cgroup v2 enables controllers for a
non-root cgroup only while no process is in it. The child joins its
cgroup before `execv()`. vinit sets `memory.max`, `pids.max` and
`cpu.max` before every start, and resets a removed limit to `max`.
Signals go to the process group and to each PID in `cgroup.procs`.
SIGKILL uses `cgroup.kill` (Linux 5.14) where it exists. vinit removes an
empty cgroup when its service exits. vinit keeps no cgroup fds open.

The service table is a static array (`MAX_SERVICES` = 64). There is no heap
allocation in the supervisor, so nothing can leak. Untouched BSS pages
cost no RSS.

## Event loop

All signals are blocked, and PID 1 reads them from a `signalfd`. The loop
does the following:

1. It calls `services_step(now)`, which makes every decision: start
   services whose dependencies are ready, stop unwanted services that
   have no active dependents, and enforce stop timeouts. This step repeats
   until nothing changes, and then returns the time to the nearest
   deadline.
2. It calls `poll()` with that deadline as the timeout. The fds polled are
   the signalfd, the control socket, and the readiness pipe of each
   service that is still starting.
3. Readiness data is read first, so a service that signals and then exits
   still counts as ready. On SIGCHLD, it calls `reap_children()`, which
   records exits and applies the restart policy. A control request goes to
   `control_handle()`.

No signal handlers run, so there are no async-signal-safety issues. The
poll set is rebuilt on every iteration. It has at most 2 + 64 entries, so
`poll()` is enough, and `epoll` would only add registration bookkeeping.
The poll timeout covers every deadline, so no `timerfd` is needed either.

If `signalfd()` fails, vinit falls back to `sigtimedwait()` and runs
without the control socket. It still polls readiness pipes, caps the poll
wait at 100 ms, and checks pending signals after processing readiness.

## Control protocol

The control socket is `/run/vinit/control`, a Unix **datagram** socket
with mode 0666. vinit only uses `/run/vinit` if it is a directory owned by
root and not writable by group or others.

A request is one datagram of the form `<command> [<service>]`. It is
printable ASCII, at most 255 bytes long. The reply is one datagram, sent
back to the sender's address. It is either `ok\n` followed by output, or
`error: <message>\n`. The client autobinds an abstract address so that it
can receive the reply.

Datagrams were chosen over `SOCK_STREAM` and `SOCK_SEQPACKET` because
PID 1 then keeps no state for each client:

- A client that connects and then stalls, or one that disappears, costs
  nothing.
- A reply that cannot be delivered is dropped (`MSG_DONTWAIT`).
- One wakeup handles at most 16 datagrams, so a flood cannot starve
  signal handling.

Every message is authenticated. With `SO_PASSCRED`, the kernel attaches
the real uid and pid of the sender. `status` and `list` are open to any
user. All other commands need uid 0. vinit logs every privileged request
with the uid and pid of the sender.

vinit closes any fds a sender passes with `SCM_RIGHTS`. The integration
test checks for this.

Each reply is limited to 8 KiB. The full service table (64 services, one
line each) fits in that.

### Reaping, and where pidfd is used

`reap_children()` first peeks with `waitid(WNOWAIT)`. While the zombie
exists, its PID, and so its process group ID, cannot be reused. This makes
it safe to `kill(-pgid, SIGKILL)` the service's leftover processes. Only
then does vinit reap the child.

Direct children cannot have PID reuse races, because PID 1 is the parent
of every service. pidfd therefore adds nothing here. vinit uses a pidfd
only for processes in a service's cgroup that are not its children: before
it sends SIGTERM or SIGCONT, it checks that the process is still in the
cgroup.

### Starting a service

1. The parent resolves the user and group with `fgetpwent()` and
   `fgetgrent()` on `/etc/passwd` and `/etc/group`. NSS is never loaded
   into PID 1.
2. The parent creates a close-on-exec pipe and calls `fork()`.
3. The child does the following, in order: reset the signal mask,
   `setsid()`, connect the `log` pipe, set stdin, `setgroups()`/`setgid()`/`setuid()`, `chdir()`
   (after the privilege drop), set the environment, `execv()`.
4. If any step fails, the child writes the step name and errno to the pipe
   and exits with 127.
5. The parent reads until EOF, which means the exec succeeded. This way an
   exec failure is reported precisely.

## Service states

| State      | Meaning                                                    |
|------------|------------------------------------------------------------|
| `inactive` | Not running. When `want` is set, the service starts or waits to restart (`deadline`). |
| `starting` | A oneshot is running, or a simple service is between fork and exec. |
| `running`  | A simple service has been executed, or a oneshot exited with status 0. |
| `stopping` | SIGTERM was sent. SIGKILL follows at `deadline`.           |
| `failed`   | A config error, a failed dependency, a failure with no restart, or the restart limit was reached. |

## Boot sequence

1. Refuse to run unless `getpid() == 1`.
2. Block all signals, set `umask 022`, `chdir /`, and set a default `PATH`.
3. Mount `/proc`, `/sys`, `/dev`, `/dev/pts`, `/run` and `/dev/shm`.
   Mount points that are already mounted are skipped. Failures are logged
   and do not stop the boot. Create the `/dev/fd`, `/dev/stdin`,
   `/dev/stdout` and `/dev/stderr` symlinks, which devtmpfs lacks.
4. If fds 0-2 are closed, fill them from `/dev/console`. If fds 1 and 2
   are the same terminal, replace fd 2 with a nonblocking, close-on-exec
   open of it for vinit's own log (see Logging).
5. Make Ctrl-Alt-Del send SIGINT.
6. Load and check the services, then enter the loop.

## Shutdown

1. All services lose `want`. Services stop in reverse dependency order,
   bounded by 30 s in total.
2. `kill(-1, SIGTERM)` and SIGCONT, then reap for up to 5 s.
3. `kill(-1, SIGKILL)`, then reap for up to 2 s.
4. `sync()`, remount `/` read-only, `reboot()`.
5. If `reboot()` fails, sleep forever. PID 1 never exits.

## Readiness

With `ready_fd = N`, `start_service()` creates a pipe. The child does the
following:

1. It moves its exec-error pipe above N with `F_DUPFD_CLOEXEC`, so that
   `dup2()` cannot overwrite it.
2. It `dup2()`s the write end of the readiness pipe to N.

The parent keeps the read end, non-blocking, in `notify`. A newline sets
the service to `running` and closes the pipe. If the pipe reaches EOF
before a newline, vinit logs it, and the service then waits for its
timeout or its exit.

On a timeout, vinit sets `timed_out` and stops the service. `finished()`
then counts the exit as a failure and does not treat it as a requested
stop.

## Reload

`services_reload()` loads the directory into a temporary table, mapped
with `mmap()` and unmapped afterwards. This way the idle RSS of PID 1 does
not grow by a second table. The reload is checked against the live table
before anything changes:

- If a service without errors now would have an error, the reload is
  refused.
- If a removed service is not `inactive` or `failed`, the reload is
  refused.

Otherwise, for each service that exists in both tables, the runtime part
of the struct is copied over, from `state` to the end. The new table is
then copied in with `copy_service()`, which re-points `argv` into the
copy's own `cmdbuf`. Dependency indices come from a fresh
`config_resolve()` on the new table. The built-in emergency shell is kept
while it runs.

`same_config()` compares the configuration part of the old and new
entries. A running service whose configuration changed gets
`restarting = 1` and is stopped, so it starts again with the new
definition. A removed service that was a log target has its pipe closed.

## Logging

`say()` is for routine events, and `warn()` is for failures. Both write one
line per `write()` to the console. `vinit.quiet` on the kernel command line
(read from `/proc/cmdline`) suppresses `say()`.

On a terminal, vinit's fd 2 is nonblocking, so a stalled serial console
cannot stall PID 1. A write to a busy console waits up to 1 s. After
that, vinit drops log lines until a write succeeds again. Services and the
shutdown hook get the blocking console as their fd 2: the child copies
fd 1 over fd 2 before `execve()`.

Messages do not go to `/dev/kmsg`. The kernel rate-limits writes to it
from userspace (`printk.devkmsg=ratelimit`) and would drop boot messages.

## Start, stop and restart semantics

- `start` sets `want` on the service and, transitively, on everything it
  requires. It also clears the failure count of a failed service. The
  service then starts in `services_step()`, once its dependencies are
  ready. A service with a config error cannot be started.
- `stop` clears `want` on the service and, transitively, on everything that
  requires it. Dependents stop first. Services that only have `after` on
  it are not affected.
- `restart` stops the process, then starts it again with no delay, and
  leaves dependents alone. A finished oneshot runs again.
- When a service exits on its own (a crash, or a timeout before it is
  ready), vinit stops every running service that `requires` it,
  transitively. Those services keep `want`. They start again when the
  service is ready again, and they fail if it fails. Finished oneshots
  and `after` dependents are not affected.

## Possible future work

These are not planned. They are listed here so the decision can be made
on purpose:

- Route service output to a logger service.
- More resource limits than `[limits]` has. Until then, wrap the
  command, for example
  `command = /usr/bin/prlimit --nofile=65536 -- /usr/sbin/nginx`.
