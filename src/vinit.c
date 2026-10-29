/* vinit - minimal init (PID 1) for Linux.
 *
 * Mounts the API filesystems, blocks every signal and reads them from a
 * signalfd, supervises the services in /etc/vinit/services, serves the
 * control socket, reaps all children (including orphans re-parented to
 * PID 1), and shuts down in order on request.
 *
 * Signals (busybox-compatible):
 *   SIGTERM, SIGINT (Ctrl-Alt-Del)   reboot
 *   SIGUSR1                          halt
 *   SIGUSR2                          power off
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vinit.h"

#ifndef SYS_close_range
#define SYS_close_range 436 /* same number on every architecture; older libc headers lack it */
#endif

#define SERVICES_TIMEOUT_MS 30000 /* bound on stopping services at shutdown */
#define TERM_TIMEOUT_MS      5000 /* grace period after SIGTERM to all */
#define KILL_TIMEOUT_MS      2000
#define HOOK_TIMEOUT_MS    120000
#define LOG_WAIT_MS          1000 /* longest wait for a busy console per line */
#define SHUTDOWN_HOOK  "/etc/vinit/shutdown"

int shutdown_how; /* 0, or the reboot() command to finish with */
static long long shutdown_deadline;

static int quiet; /* "vinit.quiet" on the kernel command line */

/* fd 2 may be nonblocking (see nonblocking_log()). Wait a while for a
 * busy console. Once it is stalled, drop lines until a write gets
 * through again. */
static void write_log(const char *buf, size_t n)
{
	static int stalled;
	struct pollfd p = { .fd = STDERR_FILENO, .events = POLLOUT };
	int waited = stalled;
	ssize_t w;

	while (n > 0) {
		if ((w = write(STDERR_FILENO, buf, n)) > 0) {
			buf += w;
			n -= (size_t)w;
			stalled = 0;
			continue;
		}
		if (w < 0 && errno == EAGAIN && !waited) {
			waited = 1;
			if (poll(&p, 1, LOG_WAIT_MS) > 0)
				continue;
		}
		if (w < 0 && errno == EAGAIN)
			stalled = 1;
		return;
	}
}

/* One write() per message so lines from concurrent processes do not mix. */
static void vlog(const char *fmt, va_list ap)
{
	char buf[512] = "vinit: ";
	size_t n = strlen(buf);
	int r;

	r = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
	if (r < 0)
		return;
	n += (size_t)r < sizeof buf - n - 1 ? (size_t)r : sizeof buf - n - 2;
	buf[n++] = '\n';
	write_log(buf, n);
}

void say(const char *fmt, ...)
{
	va_list ap;

	if (quiet)
		return;
	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

void warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

static void read_cmdline(void)
{
	char buf[4096];
	ssize_t n;
	int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = '\0';
	for (char *t = strtok(buf, " \n"); t; t = strtok(NULL, " \n"))
		if (strcmp(t, "vinit.quiet") == 0)
			quiet = 1;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* timeout_ms < 0 waits forever. Returns the signal number, or -1. */
static int wait_signal(const sigset_t *set, long long timeout_ms, siginfo_t *si)
{
	struct timespec ts;

	if (timeout_ms < 0)
		return sigwaitinfo(set, si);
	ts.tv_sec = timeout_ms / 1000;
	ts.tv_nsec = (timeout_ms % 1000) * 1000000;
	return sigtimedwait(set, si, &ts);
}

static int is_mountpoint(const char *dir)
{
	char parent[64];
	struct stat a, b;

	snprintf(parent, sizeof parent, "%s/..", dir);
	return stat(dir, &a) == 0 && stat(parent, &b) == 0 && a.st_dev != b.st_dev;
}

static void mount_api_filesystems(void)
{
	static const struct {
		const char *type, *dir, *data;
		unsigned long flags;
	} fs[] = {
		{ "proc",     "/proc",    NULL,               MS_NOSUID | MS_NODEV | MS_NOEXEC },
		{ "sysfs",    "/sys",     NULL,               MS_NOSUID | MS_NODEV | MS_NOEXEC },
		{ "devtmpfs", "/dev",     "mode=0755",        MS_NOSUID },
		{ "devpts",   "/dev/pts", "gid=5,mode=0620",  MS_NOSUID | MS_NOEXEC },
		{ "tmpfs",    "/run",     "mode=0755",        MS_NOSUID | MS_NODEV },
		{ "tmpfs",    "/dev/shm", "mode=1777",        MS_NOSUID | MS_NODEV },
	};
	/* devtmpfs does not create these; bash needs /dev/fd for <(...). */
	static const char *const links[][2] = {
		{ "/proc/self/fd",   "/dev/fd" },
		{ "/proc/self/fd/0", "/dev/stdin" },
		{ "/proc/self/fd/1", "/dev/stdout" },
		{ "/proc/self/fd/2", "/dev/stderr" },
	};

	for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) {
		if (mkdir(fs[i].dir, 0755) < 0 && errno != EEXIST)
			warn("mkdir %s: %s", fs[i].dir, strerror(errno));
		/* The kernel or an initramfs may have mounted it already. */
		if (is_mountpoint(fs[i].dir))
			continue;
		if (mount(fs[i].type, fs[i].dir, fs[i].type, fs[i].flags, fs[i].data) < 0)
			warn("mount %s on %s: %s", fs[i].type, fs[i].dir, strerror(errno));
	}
	for (size_t i = 0; i < sizeof links / sizeof links[0]; i++)
		(void)!symlink(links[i][0], links[i][1]);
	/* Optional: without cgroup v2, services run in process groups only.
	 * The mount fails in a user namespace without a cgroup namespace. */
	if (!is_mountpoint(CGROUP_ROOT) &&
	    mount("cgroup2", CGROUP_ROOT, "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
		say("mount cgroup2 on %s: %s", CGROUP_ROOT, strerror(errno));
}

/* The kernel leaves fds 0-2 closed when the initramfs has no /dev/console.
 * Fill the gaps now that devtmpfs is mounted, so later open() calls never
 * land on 0-2 by accident. */
static void setup_stdio(void)
{
	int fd, i;

	for (i = 0; i < 3 && fcntl(i, F_GETFD) >= 0; i++)
		;
	if (i == 3)
		return;
	fd = open("/dev/console", O_RDWR | O_NOCTTY);
	if (fd < 0)
		fd = open("/dev/null", O_RDWR);
	if (fd < 0)
		return;
	for (i = 0; i < 3; i++)
		if (fcntl(i, F_GETFD) < 0)
			dup2(fd, i);
	if (fd > 2)
		close(fd);
}

/* A stalled serial console must not stall PID 1. When stdout and stderr
 * are the same terminal, log through a separate nonblocking open of it on
 * fd 2 (see write_log()). fd 2 is close-on-exec, so
 * children put the blocking fd 1 back as their fd 2. */
static void nonblocking_log(void)
{
	struct stat out, err;
	int fd;

	if (!isatty(2) || fstat(1, &out) < 0 || fstat(2, &err) < 0 || out.st_rdev != err.st_rdev)
		return;
	fd = open("/proc/self/fd/2", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	dup3(fd, 2, O_CLOEXEC);
	close(fd);
}

/* Returns 1 when all children are gone, 0 on timeout. A SIGCHLD that
 * arrives between reap_children() and the wait stays pending, so none is
 * lost. */
static int wait_children(const sigset_t *chld, long long timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;
	siginfo_t si;

	while (reap_children(now_ms())) {
		long long left = deadline - now_ms();

		if (left <= 0)
			return 0;
		wait_signal(chld, left, &si);
	}
	return 1;
}

/* Distribution shutdown work (hardware clock, RNG seed, LVM, ...), e.g.
 * Void's /etc/runit/3. It runs in PID 1's session, which tools such as
 * Void's "pkill --inverse -s0,1" spare. */
static void run_shutdown_hook(const sigset_t *chld, const char *what)
{
	long long deadline = now_ms() + HOOK_TIMEOUT_MS;
	siginfo_t si;
	pid_t pid;

	if (access(SHUTDOWN_HOOK, X_OK) < 0)
		return;
	say("running %s %s", SHUTDOWN_HOOK, what);
	if ((pid = fork()) < 0) {
		warn("fork: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (fcntl(2, F_GETFD) == FD_CLOEXEC)
			dup2(1, 2); /* the blocking console, see nonblocking_log() */
		execl(SHUTDOWN_HOOK, SHUTDOWN_HOOK, what, (char *)NULL);
		warn("exec %s: %s", SHUTDOWN_HOOK, strerror(errno));
		_exit(127);
	}
	/* Children the hook kills are reaped too; once reap_children() has
	 * taken the hook itself, waitpid() reports ECHILD. */
	while (waitpid(pid, NULL, WNOHANG) == 0) {
		long long left = deadline - now_ms();

		if (left <= 0) {
			warn("%s did not finish in %d s, killing it", SHUTDOWN_HOOK,
			     HOOK_TIMEOUT_MS / 1000);
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
			break;
		}
		wait_signal(chld, left, &si);
		reap_children(now_ms());
	}
}

/* Undo the octal escapes of /proc/self/mounts and /proc/swaps in place. */
static void unescape(char *s)
{
	char *o = s;

	for (; *s; s++, o++) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
		    s[3] >= '0' && s[3] <= '7') {
			*o = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
			s += 3;
		} else {
			*o = *s;
		}
	}
	*o = '\0';
}

/* Read a whole /proc file and return the second line onwards, or the
 * first line if skip_header is 0. */
static char *read_proc(const char *path, char *buf, size_t len, int skip_header)
{
	size_t n = 0;
	ssize_t r;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	while (n < len - 1 && (r = read(fd, buf + n, len - 1 - n)) > 0)
		n += (size_t)r;
	close(fd);
	buf[n] = '\0';
	if (!skip_header)
		return buf;
	return strchr(buf, '\n') ? strchr(buf, '\n') + 1 : buf + n;
}

/* Safety net after the hook: turn off swap, then unmount everything but /
 * in reverse mount order (children first). A filesystem that stays busy
 * is remounted read-only, so no data is left unwritten. */
static void release_filesystems(void)
{
	static char buf[65536];
	char *dirs[512], *line, *next;
	int n = 0;

	if ((line = read_proc("/proc/swaps", buf, sizeof buf, 1)))
		for (; *line; line = next) {
			next = strchr(line, '\n') ? strchr(line, '\n') + 1 : line + strlen(line);
			line[strcspn(line, " \t\n")] = '\0';
			unescape(line);
			if (*line && swapoff(line) < 0)
				warn("swapoff %s: %s", line, strerror(errno));
		}

	if (!(line = read_proc("/proc/self/mounts", buf, sizeof buf, 0)))
		return;
	say("unmounting filesystems");
	/* Each line: "source target fstype options 0 0". */
	for (; *line && n < (int)(sizeof dirs / sizeof dirs[0]); line = next) {
		char *target;

		next = strchr(line, '\n') ? strchr(line, '\n') + 1 : line + strlen(line);
		if (!(target = strchr(line, ' ')))
			continue;
		target++;
		target[strcspn(target, " \n")] = '\0';
		unescape(target);
		dirs[n++] = target;
	}
	while (n-- > 0) {
		if (strcmp(dirs[n], "/") == 0 || umount2(dirs[n], 0) == 0)
			continue;
		if (mount(NULL, dirs[n], NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0)
			warn("cannot unmount or remount read-only %s: %s", dirs[n], strerror(errno));
	}
}

/* Services are stopped already; deal with everything else. */
static _Noreturn void final_shutdown(int how)
{
	const char *what = how == RB_AUTOBOOT ? "reboot" :
			   how == RB_POWER_OFF ? "poweroff" : "halt";
	sigset_t chld;

	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);

	run_shutdown_hook(&chld, what);

	say("sending SIGTERM to all processes");
	kill(-1, SIGTERM);
	kill(-1, SIGCONT); /* stopped processes must run to act on SIGTERM */
	if (!wait_children(&chld, TERM_TIMEOUT_MS)) {
		say("sending SIGKILL to remaining processes");
		kill(-1, SIGKILL);
		if (!wait_children(&chld, KILL_TIMEOUT_MS))
			warn("some processes did not exit");
	}

	sync();
	release_filesystems();
	sync();
	if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0)
		warn("remount / read-only: %s", strerror(errno));

	say("%s", how == RB_AUTOBOOT ? "rebooting" :
		  how == RB_POWER_OFF ? "powering off" : "halting");
	reboot(how);

	/* PID 1 must never exit: the kernel panics if it does. */
	warn("reboot: %s", strerror(errno));
	for (;;)
		pause();
}

void request_shutdown(int how)
{
	if (shutdown_how)
		return;
	shutdown_how = how;
	shutdown_deadline = now_ms() + SERVICES_TIMEOUT_MS;
	say("shutdown requested, stopping services");
}

static void handle_signal(int sig)
{
	switch (sig) {
	case SIGCHLD:
		reap_children(now_ms());
		break;
	case SIGTERM:
	case SIGINT:
		request_shutdown(RB_AUTOBOOT);
		break;
	case SIGUSR1:
		request_shutdown(RB_HALT_SYSTEM);
		break;
	case SIGUSR2:
		request_shutdown(RB_POWER_OFF);
		break;
	}
}

int main(void)
{
	struct pollfd pfd[2 + MAX_SERVICES];
	struct signalfd_siginfo ssi;
	int sfd, cfd, nready;
	sigset_t all;
	siginfo_t si;
	char *term;

	if (getpid() != 1) {
		fprintf(stderr, "vinit: must run as PID 1\n");
		return 1;
	}

	/* Every signal is handled synchronously below; no handlers run. */
	sigfillset(&all);
	sigprocmask(SIG_BLOCK, &all, NULL);
	/* Fds inherited from a parent, such as a container runtime, would
	 * leak into every service. close_range() needs Linux 5.9. */
	if (syscall(SYS_close_range, 3, ~0U, 0) < 0)
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
	umask(022);
	if (chdir("/") < 0)
		warn("chdir /: %s", strerror(errno));
	/* The kernel passes unknown command line words with '=' as
	 * environment. Services get only PATH, TERM and their own env lines.
	 * clearenv() drops the array but not the strings, so term stays valid. */
	term = getenv("TERM");
	clearenv();
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
	if (term)
		setenv("TERM", term, 1);

	mount_api_filesystems();
	setup_stdio();
	nonblocking_log();
	read_cmdline();
	say("booting");

	/* Ctrl-Alt-Del now sends SIGINT instead of rebooting at once.
	 * Fails harmlessly inside a PID namespace. */
	reboot(RB_DISABLE_CAD);

	/* Without a signalfd, fall back to sigtimedwait() and run without
	 * the control socket. */
	sfd = signalfd(-1, &all, SFD_CLOEXEC | SFD_NONBLOCK);
	if (sfd < 0)
		warn("signalfd: %s; no control socket", strerror(errno));
	cfd = sfd < 0 ? -1 : control_open();
	pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
	pfd[1] = (struct pollfd){ .fd = cfd, .events = POLLIN }; /* poll() skips fd -1 */

	cgroup_init();
	services_load(SERVICE_DIR);

	for (;;) {
		long long now = now_ms();
		long long timeout = services_step(now, shutdown_how != 0);

		if (shutdown_how) {
			if (!services_active() || now >= shutdown_deadline)
				final_shutdown(shutdown_how);
			if (timeout < 0 || timeout > shutdown_deadline - now)
				timeout = shutdown_deadline - now;
		}

		/* Blocked signals cannot wake poll without signalfd. Bound the
		 * fallback wait so signals and readiness both keep progressing. */
		if (sfd < 0 && (timeout < 0 || timeout > 100))
			timeout = 100;
		/* Readiness pipes of starting services follow the two fixed fds. */
		nready = services_pollfds(pfd + 2);
		if (poll(pfd, (nfds_t)(2 + nready), timeout > INT_MAX ? INT_MAX : (int)timeout) < 0) {
			if (errno != EINTR)
				warn("poll: %s", strerror(errno));
			continue;
		}
		/* Readiness first: a service that signals and exits right away
		 * still counts as ready. */
		services_poll_events(pfd + 2, nready);
		if (sfd < 0)
			while ((nready = wait_signal(&all, 0, &si)) > 0)
				handle_signal(nready);
		if (pfd[0].revents)
			while (read(sfd, &ssi, sizeof ssi) == (ssize_t)sizeof ssi)
				handle_signal((int)ssi.ssi_signo);
		if (pfd[1].revents)
			control_handle(cfd);
	}
}
