# vinit security notes

This is a self-review by the authors of the code, written on 2026-10-24.
It is **not** an independent audit.

## Trust boundaries

| Input                         | Who controls it        | Handling |
|-------------------------------|------------------------|----------|
| `/etc/vinit/services/*.conf`  | root                   | The directory and each file must be owned by root and not writable by group or others; otherwise vinit ignores them. The parser is strict, and it is fuzzed. |
| Control socket requests       | any local user         | Kernel-verified credentials (`SO_PASSCRED`). Only uid 0 may change state. Requests are printable ASCII, at most 255 bytes. Any passed fds are closed. At most 16 requests are handled per wakeup. |
| Readiness pipe data           | the service            | Only a newline has meaning. At most 16 reads per wakeup. The pipe closes after readiness. |
| Kernel command line           | bootloader             | Only the `vinit.quiet` token is read. `KEY=value` entries reach vinit's environment through the kernel. vinit clears its environment at boot, except `TERM`, and sets `PATH`. |
| Signals to PID 1              | root (or the kernel)   | All signals are blocked and read from a signalfd. Signals without an action are ignored. This is tested for all catchable signals. |
| `/etc/passwd`, `/etc/group`   | root                   | Read with `fgetpwent()`/`fgetgrent()`. NSS is never loaded into PID 1. |

## Measures

- **No shell.** vinit never runs a command through a shell. It rejects
  shell syntax, so a service must ask for `/bin/sh -c` explicitly.
- **Privilege drop.** The order is `setgroups()`, then `setgid()`, then
  `setuid()`, and any failure aborts the start. `chdir()` comes after the
  drop, so the service user needs access to the working directory.
- **fd hygiene.** Every fd vinit opens is close-on-exec. Services get only
  fds 0-2, plus `ready_fd` if configured. The tests check that PID 1 still
  has exactly 5 fds after 100 restarts, after clients pass it fds, and
  after the error paths run.
- **PID reuse.** vinit kills a process group only while its leader is an
  unreaped child: it peeks with `waitid(WNOWAIT)` before reaping. Direct
  children cannot be subject to PID reuse races.
- **No async signal handlers.**
- **Bounded work per event** (control requests, readiness reads) and a
  backoff floor, so `delay_ms = 0` cannot make PID 1 spin.
- **No exit.** PID 1 never exits. If `reboot()` fails, it sleeps forever.
- **Hardened build.** `_FORTIFY_SOURCE=2` (glibc) and
  `-fstack-protector-strong`.

## Analysis done

- **gcc 15 `-fanalyzer`:**
  - One finding in `vinit.c`. It is a false positive: `setup_stdio()`
    keeps the console fd on purpose as stdin/stdout/stderr.
  - gcc's analyzer crashed with an internal compiler error on
    `service.c`, so that file is covered by clang only.
- **clang 21 `--analyze`:** two findings in `config.c`. Both were
  unreachable under the existing invariants, and both now have guards.
- **libFuzzer** with ASan and UBSan on the parser and resolver: 3.27
  million executions with no findings. The harness checks the output
  contract, not only crashes.
- **Manual review.** This found and fixed three issues:
  - a readiness-pipe flood could starve PID 1
  - a failing `waitpid()` after a successful `waitid()` peek could loop
    forever
  - there was no ownership check on the services directory
- **Error-path review.** This found and fixed an fd leak when `fork()`
  fails.
- **Mutation testing.** Six deliberate bugs were each caught by the
  integration test:
  - reaping only the console process, so orphans are never reaped
    (Phase 1)
  - ignoring dependencies at start
  - ignoring dependents at stop
  - not closing fds passed over the control socket
  - marking `ready_fd` services ready at exec
  - not killing a dead service's process group

## Known gaps

- **Lost log lines.** When the console is a terminal, vinit logs through
  a nonblocking fd. It waits at most 1 s for a busy console, then drops
  lines until the console drains. On a stalled serial console, failures
  can go unreported. When the console is not a terminal (for example a
  pipe from a container runtime), writes still block.
- **No sandboxing.** There are no namespaces, seccomp or capability
  bounding for services. Isolation is the uid/gid switch, plus the
  memory, process and CPU limits in `[limits]` under cgroup v2. `PR_SET_NO_NEW_PRIVS` is not set, because that would
  break getty, login and su.
- **Request flooding.** Any local user can flood the control socket. Work
  per wakeup is bounded, but a sustained flood keeps vinit busy answering
  `status` and `list`.
- **Escaping a process group.** With cgroup v2, a service's processes
  cannot escape: stopping it signals every process in its cgroup. Without
  cgroup v2, a child that calls `setsid()` or `setpgid()` escapes its
  process group. Such processes are only killed at the final `kill(-1)`
  during shutdown.
- **PID reuse in a cgroup.** SIGTERM and SIGCONT go to each PID read from
  `cgroup.procs`. A process that exits between the read and the `kill()`
  frees its PID for reuse, so the signal could reach an unrelated
  process. SIGKILL uses `cgroup.kill` where it exists (Linux 5.14), which
  has no such race.
- **Hardware coverage.** x86_64 and ARM64 passed the normal and quiet QEMU
  boot/shutdown suites with Linux 6.18.59. Real hardware remains untested.
