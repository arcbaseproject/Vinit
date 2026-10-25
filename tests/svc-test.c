/* Test service for the vinit integration tests. The first argument picks
 * the behavior; each file in tests/services uses one mode. Every line it
 * prints starts with "svc-test <mode>:" so tests/checks.sh can find it.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *mode = "?";
static int failed;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
	va_list ap;

	printf("svc-test %s: ", mode);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	if (strncmp(fmt, "FAIL", 4) == 0)
		failed++;
}

/* Run vinitctl as uid (or as we are, if uid < 0) and capture its output. */
static int run_ctl(int uid, char *buf, size_t len, const char *a1, const char *a2)
{
	int p[2], st;
	size_t n = 0;
	ssize_t r;
	pid_t pid;

	if (pipe(p) < 0)
		return -1;
	if ((pid = fork()) == 0) {
		dup2(p[1], 1);
		dup2(p[1], 2);
		if (uid >= 0 && (setgroups(0, NULL) < 0 || setgid((gid_t)uid) < 0 || setuid((uid_t)uid) < 0))
			_exit(99);
		execl("/sbin/vinitctl", "vinitctl", a1, a2, (char *)NULL);
		_exit(98);
	}
	close(p[1]);
	while (n < len - 1 && (r = read(p[0], buf + n, len - 1 - n)) > 0)
		n += (size_t)r;
	buf[n] = '\0';
	close(p[0]);
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Expect exit status want and output containing text. */
static void ctl_as(int uid, int want, const char *text, const char *a1, const char *a2)
{
	char buf[8192];
	int st = run_ctl(uid, buf, sizeof buf, a1, a2);

	if (st != want || !strstr(buf, text))
		out("FAIL vinitctl %s %s as %d: exit %d, want %d with '%s', got: %s", a1, a2 ? a2 : "",
		    uid, st, want, text, buf);
	else
		out("vinitctl %s%s%s: ok", a1, a2 ? " " : "", a2 ? a2 : "");
}

#define ctl(want, text, a1, a2) ctl_as(-1, want, text, a1, a2)

/* Poll "vinitctl status name" until it shows state, for up to 5 s. */
static void wait_state(const char *name, const char *state)
{
	char buf[8192], want[64];

	snprintf(want, sizeof want, "state: %s\n", state);
	for (int i = 0; i < 100; i++) {
		if (run_ctl(-1, buf, sizeof buf, "status", name) == 0 && strstr(buf, want)) {
			out("%s is %s", name, state);
			return;
		}
		usleep(50000);
	}
	out("FAIL %s did not become %s: %s", name, state, buf);
}

/* Send one raw datagram (optionally passing fds) and return the reply. */
static void raw_request(const void *req, size_t len, int pass_fds, char *reply, size_t rlen)
{
	struct sockaddr_un srv = { .sun_family = AF_UNIX, .sun_path = "/run/vinit/control" };
	sa_family_t autobind = AF_UNIX;
	struct timeval tv = { .tv_sec = 5 };
	union {
		struct cmsghdr align;
		char buf[CMSG_SPACE(3 * sizeof(int))];
	} cm;
	struct iovec iov = { (void *)req, len };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0), fds[3];
	ssize_t n;

	reply[0] = '\0';
	if (fd < 0 || bind(fd, (struct sockaddr *)&autobind, sizeof autobind) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0 ||
	    connect(fd, (struct sockaddr *)&srv, sizeof srv) < 0)
		return;
	if (pass_fds) {
		struct cmsghdr *h;

		for (int i = 0; i < 3; i++)
			fds[i] = open("/proc/self/stat", O_RDONLY);
		mh.msg_control = cm.buf;
		mh.msg_controllen = sizeof cm.buf;
		h = CMSG_FIRSTHDR(&mh);
		h->cmsg_level = SOL_SOCKET;
		h->cmsg_type = SCM_RIGHTS;
		h->cmsg_len = CMSG_LEN(sizeof fds);
		memcpy(CMSG_DATA(h), fds, sizeof fds);
	}
	if (sendmsg(fd, &mh, 0) >= 0 && (n = recv(fd, reply, rlen - 1, 0)) >= 0)
		reply[n] = '\0';
	close(fd);
}

static void raw(const void *req, size_t len, int pass_fds, const char *want)
{
	char reply[8192];

	raw_request(req, len, pass_fds, reply, sizeof reply);
	if (strncmp(reply, want, strlen(want)) != 0)
		out("FAIL raw request: want '%s', got '%s'", want, reply);
	else
		out("raw request -> %.*s: ok", (int)strcspn(reply, "\n"), reply);
}

/* utime + stime of PID 1, in clock ticks. */
static long pid1_ticks(void)
{
	char buf[1024], *p;
	long ut = -1, st = -1;
	FILE *f = fopen("/proc/1/stat", "r");
	size_t len;

	if (!f)
		return -1;
	len = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[len] = '\0';
	/* After "(comm)": state is field 3; utime and stime are 14 and 15. */
	if ((p = strrchr(buf, ')')))
		sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &ut, &st);
	return ut < 0 || st < 0 ? -1 : ut + st;
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (!f || fputs(text, f) < 0 || fclose(f) != 0)
		out("FAIL cannot write %s", path);
}

#define SVC_DIR "/etc/vinit/services/"

static void reload_tests(void)
{
	const char *base = "[service]\ncommand = /bin/svc-test daemon base\n";

	/* A new service is added and started. */
	write_file(SVC_DIR "late.conf", "[service]\ncommand = /bin/svc-test daemon late\n");
	ctl(0, "1 added, 0 removed", "reload", NULL);
	wait_state("late", "running");

	/* Breaking a working service is refused; nothing changes. */
	write_file(SVC_DIR "base.conf", "[service]\ncommand = /bin/svc-test daemon base\ncolour = red\n");
	ctl(1, "service base: /etc/vinit/services/base.conf:3: unknown key 'colour'", "reload", NULL);
	ctl(0, "state: running", "status", "base");
	write_file(SVC_DIR "base.conf", base);

	/* Removing an active service is refused until it is stopped. */
	unlink(SVC_DIR "late.conf");
	ctl(1, "late was removed but is still active; stop it first", "reload", NULL);
	ctl(0, "", "stop", "late");
	wait_state("late", "inactive");
	ctl(0, "0 added, 1 removed", "reload", NULL);
	ctl(1, "unknown service 'late'", "status", "late");

	/* A fixed service starts. */
	write_file(SVC_DIR "missing.conf", "[service]\ncommand = /bin/svc-test daemon missing\n");
	ctl(0, "0 added, 0 removed", "reload", NULL);
	wait_state("missing", "running");
}

static int count_fds(void)
{
	DIR *d = opendir("/proc/1/fd");
	struct dirent *e;
	int n = 0;

	while (d && (e = readdir(d)))
		n += e->d_name[0] != '.';
	if (d)
		closedir(d);
	return n;
}

static long rss_kb(void)
{
	char buf[256];
	long kb = -1;
	FILE *f = fopen("/proc/1/status", "r");

	while (f && fgets(buf, sizeof buf, f))
		if (sscanf(buf, "VmRSS: %ld", &kb) == 1)
			break;
	if (f)
		fclose(f);
	return kb;
}

static void stress_tests(void)
{
	static const int skip[] = { SIGKILL, SIGSTOP, SIGTERM, SIGINT, SIGUSR1, SIGUSR2, SIGCHLD };
	int fds, ok = 0;
	long rss;
	FILE *f;
	int child = 0;

	/* The leftover child of a dead simple service must be gone. */
	if ((f = fopen("/run/leaky.child", "r"))) {
		if (fscanf(f, "%d", &child) != 1)
			child = 0;
		fclose(f);
	}
	if (child > 0 && kill(child, 0) < 0)
		out("leaky child %d is gone", child);
	else
		out("FAIL leaky child %d still exists", child);

	/* PID 1 must survive every signal it has no action for. */
	for (int sig = 1; sig < SIGRTMIN + 4; sig++) {
		int act = 0;

		for (size_t k = 0; k < sizeof skip / sizeof skip[0]; k++)
			act |= skip[k] == sig;
		if ((!act && sig < 32) || sig >= SIGRTMIN)
			kill(1, sig);
	}
	usleep(100000);
	ctl(0, "up, 25 services", "status", NULL);

	/* 50 clients at once must all get answers. */
	for (int i = 0; i < 50; i++)
		if (fork() == 0) {
			char buf[8192];

			_exit(run_ctl(-1, buf, sizeof buf, "list", NULL));
		}
	for (int i = 0; i < 50; i++) {
		int st;

		if (wait(&st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0)
			ok++;
	}
	if (ok == 50)
		out("50 concurrent clients: ok");
	else
		out("FAIL only %d of 50 concurrent clients succeeded", ok);

	/* 100 restarts must not leak fds or memory in PID 1. */
	fds = count_fds();
	rss = rss_kb();
	for (int i = 0; i < 100; i++) {
		char buf[8192];

		run_ctl(-1, buf, sizeof buf, "restart", "churn");
		for (int t = 0; t < 100; t++) {
			usleep(10000);
			if (run_ctl(-1, buf, sizeof buf, "status", "churn") == 0 &&
			    strstr(buf, "state: running\n") && !strstr(buf, "last exit: none"))
				break;
		}
	}
	out("after 100 restarts: fds %d -> %d, RSS %ld -> %ld kB", fds, count_fds(), rss, rss_kb());
	if (count_fds() != fds || rss_kb() > rss + 64)
		out("FAIL PID 1 leaked during restarts");
}

static void control_tests(void)
{
	char big[300];

	ctl(0, "crashloop", "list", NULL);
	ctl(0, "state: running", "status", "base");
	ctl(0, "state: failed", "status", "crashloop");
	ctl(0, "last exit: signal 6 (Aborted)", "status", "crashloop");
	ctl(0, "up, 25 services", "status", NULL);
	ctl(0, "state: running", "status", "after-ready");
	ctl(0, "state: failed", "status", "never-ready");
	ctl(1, "unknown service 'nope'", "start", "nope");
	ctl(1, "requires 'nonexistent' is not a known service", "start", "missing");
	ctl(1, "unknown command 'bogus'", "bogus", NULL);
	ctl(1, "usage: start service", "start", NULL);
	ctl_as(65534, 1, "permission denied", "stop", "base");
	ctl_as(65534, 0, "base", "list", NULL);

	/* Stopping base must stop setup and app (they require it) first. */
	ctl(0, "", "stop", "base");
	wait_state("app", "inactive");
	wait_state("base", "inactive");
	ctl(0, "state: inactive", "status", "setup");

	/* Starting app must bring its requirements back, in order. */
	ctl(0, "", "start", "app");
	wait_state("app", "running");
	ctl(0, "state: running", "status", "base");

	ctl(0, "", "restart", "flaky");
	wait_state("flaky", "running");

	raw("\x01\x02", 2, 0, "error: malformed request");
	raw("", 0, 0, "error: empty request");
	memset(big, 'a', sizeof big);
	raw(big, sizeof big, 0, "error: request too long");
	raw("list", 4, 1, "ok\n");

	reload_tests();
	stress_tests();
}

/* Wait for SIGTERM, report it, exit 0 after a short delay. The delay
 * makes a wrong stop order visible: a dependency must not get SIGTERM
 * before its dependent has exited. */
static _Noreturn void daemon_loop(void)
{
	sigset_t set;
	int sig;

	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, NULL);
	sigwait(&set, &sig);
	out("got SIGTERM");
	usleep(200000);
	out("exiting");
	exit(0);
}

static int count_zombies(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char path[300], buf[512], *p;
		size_t len;
		FILE *f;

		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
		if (!(f = fopen(path, "r")))
			continue;
		len = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		buf[len] = '\0';
		/* Format: "pid (comm) S ..."; comm may contain spaces or ')'. */
		p = strrchr(buf, ')');
		if (p && p[1] == ' ' && p[2] == 'Z')
			n++;
	}
	closedir(d);
	return n;
}

static void print_line(const char *path, const char *prefix)
{
	char buf[256];
	FILE *f = fopen(path, "r");

	if (!f)
		return;
	while (fgets(buf, sizeof buf, f))
		if (strncmp(buf, prefix, strlen(prefix)) == 0) {
			buf[strcspn(buf, "\n")] = '\0';
			out("%s %s", path, buf);
		}
	fclose(f);
}

static int flaky(void)
{
	const char *state = "/run/flaky.state";
	long long first;
	FILE *f = fopen(state, "r");

	if (!f) {
		if (!(f = fopen(state, "w"))) {
			out("FAIL cannot write %s", state);
			return 1;
		}
		fprintf(f, "%lld\n", now_ms());
		fclose(f);
		out("first run, exiting with status 3");
		return 3;
	}
	if (fscanf(f, "%lld", &first) != 1)
		first = 0;
	fclose(f);
	out("respawned after %lld ms", now_ms() - first);
	if (now_ms() - first < 900)
		out("FAIL restart delay too short");
	else
		out("ok");
	daemon_loop();
}

static int check(void)
{
	int zombies, ready[2];
	sigset_t term;
	char c;

	/* SIGTERM may come right after the poweroff request below. */
	sigemptyset(&term);
	sigaddset(&term, SIGTERM);
	sigprocmask(SIG_BLOCK, &term, NULL);

	/* Each grandchild outlives its parent, is re-parented to PID 1, then
	 * exits. Only PID 1 can reap it. */
	for (int i = 0; i < 20; i++) {
		pid_t mid = fork();

		if (mid == 0) {
			if (fork() == 0)
				usleep(50000);
			_exit(0);
		}
		if (mid > 0)
			waitpid(mid, NULL, 0);
	}
	usleep(500000);
	zombies = count_zombies();
	out("zombies after orphans exit: %d", zombies);

	/* An orphan outside any service's process group that ignores
	 * SIGTERM: the final shutdown must SIGKILL it. Wait until it is set
	 * up, or stopping this service could kill it first. */
	if (pipe(ready) < 0)
		out("FAIL pipe");
	if (fork() == 0) {
		if (fork() == 0) {
			setsid();
			signal(SIGTERM, SIG_IGN);
			(void)!write(ready[1], "x", 1);
			for (;;)
				pause();
		}
		_exit(0);
	}
	close(ready[1]);
	if (read(ready[0], &c, 1) != 1)
		out("FAIL orphan setup");

	{
		DIR *d = opendir("/proc/1/fd");
		struct dirent *e;
		int n = 0;

		while (d && (e = readdir(d)))
			n += e->d_name[0] != '.';
		if (d)
			closedir(d);
		out("PID 1 open fds: %d", n);
	}
	{
		long before = pid1_ticks();

		sleep(3);
		out("PID 1 CPU ticks during 3 s idle: %ld (%ld per second)", pid1_ticks() - before,
		    sysconf(_SC_CLK_TCK));
	}
	control_tests();
	{
		DIR *d = opendir("/proc/1/fd");
		struct dirent *e;
		int n = 0;

		while (d && (e = readdir(d)))
			n += e->d_name[0] != '.';
		if (d)
			closedir(d);
		out("PID 1 open fds after control tests: %d", n);
	}
	print_line("/proc/1/status", "VmRSS");
	print_line("/proc/uptime", "");
	out(zombies == 0 && !failed ? "PASS" : "FAIL");
	ctl(0, "", "poweroff", NULL);
	daemon_loop();
}

int main(int argc, char **argv)
{
	struct stat st;

	/* Installed as /etc/vinit/shutdown too: the shutdown hook. */
	if (strstr(argv[0], "shutdown")) {
		mode = "hook";
		out("called with %s", argc > 1 ? argv[1] : "(nothing)");
		out("same session as PID 1: %s", getsid(0) == getsid(1) ? "yes" : "no");
		return 0;
	}
	if (argc < 2)
		return 2;
	mode = argv[argc > 2 ? 2 : 1];

	if (strcmp(argv[1], "daemon") == 0) {
		daemon_loop();
	} else if (strcmp(argv[1], "setup") == 0) {
		usleep(300000);
		fclose(fopen("/run/setup.done", "w"));
		out("done");
	} else if (strcmp(argv[1], "app") == 0) {
		out(stat("/run/setup.done", &st) == 0 ? "dependencies ready" :
		    "FAIL started before setup finished");
		daemon_loop();
	} else if (strcmp(argv[1], "flaky") == 0) {
		return flaky();
	} else if (strcmp(argv[1], "stubborn") == 0) {
		signal(SIGTERM, SIG_IGN);
		for (;;)
			pause();
	} else if (strcmp(argv[1], "crash") == 0) {
		abort();
	} else if (strcmp(argv[1], "id") == 0) {
		char cwd[256] = "?";
		const char *g = getenv("GREETING");

		(void)!getcwd(cwd, sizeof cwd);
		out("uid=%d gid=%d groups=%d", (int)getuid(), (int)getgid(), getgroups(0, NULL));
		out("cwd=%s GREETING=%s", cwd, g ? g : "(unset)");
	} else if (strcmp(argv[1], "notify") == 0) {
		usleep(500000);
		fclose(fopen("/run/ready-slow.ready", "w"));
		if (write(3, "\n", 1) != 1)
			out("FAIL write to ready_fd 3");
		close(3);
		daemon_loop();
	} else if (strcmp(argv[1], "after-ready") == 0) {
		out(stat("/run/ready-slow.ready", &st) == 0 ? "dependency was ready" :
		    "FAIL started before its dependency was ready");
		daemon_loop();
	} else if (strcmp(argv[1], "exit") == 0) {
		return 0;
	} else if (strcmp(argv[1], "sleeper") == 0) {
		for (;;)
			pause();
	} else if (strcmp(argv[1], "leaky") == 0) {
		pid_t c = fork();
		FILE *f;

		if (c == 0)
			for (;;)
				pause();
		if ((f = fopen("/run/leaky.child", "w"))) {
			fprintf(f, "%d\n", (int)c);
			fclose(f);
		}
		out("left child %d behind", (int)c);
		return 1;
	} else if (strcmp(argv[1], "uptime") == 0) {
		print_line("/proc/uptime", "");
	} else if (strcmp(argv[1], "check") == 0) {
		return check();
	} else {
		return 2;
	}
	return 0;
}
