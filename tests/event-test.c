/* Exercise PID 1's signalfd fallback without mounts or a PID namespace. */
#define _GNU_SOURCE
#include <poll.h>
#include <unistd.h>
static int test_poll(struct pollfd *, nfds_t, int);
static ssize_t test_write(int, const void *, size_t);
pid_t test_getpid(void);
int test_chdir(const char *);
#define main vinit_main
#define getpid test_getpid
#define signalfd test_signalfd
#define mount test_mount
#define mkdir test_mkdir
#define chdir test_chdir
#define reboot test_reboot
#define poll test_poll
#define write test_write
#include "../src/vinit.c"
#undef main
#undef getpid
#undef signalfd
#undef mount
#undef mkdir
#undef chdir
#undef reboot
#undef poll
#undef write
#include <assert.h>
#include <setjmp.h>

static jmp_buf done;
static int readiness[2], ready_seen, child_seen, steps;
static int log_test, log_polls, log_writes;

static ssize_t test_write(int fd, const void *buf, size_t n)
{
	if (!log_test)
		return write(fd, buf, n);
	assert(fd == STDERR_FILENO);
	assert(++log_writes <= 3);
	if (log_test == 2)
		return (ssize_t)n;
	errno = EAGAIN;
	return -1;
}

pid_t test_getpid(void) { return 1; }
int test_signalfd(int fd, const sigset_t *set, int flags)
{
	(void)fd; (void)set; (void)flags;
	errno = ENOSYS;
	return -1;
}
int test_mount(const char *source, const char *target, const char *type,
               unsigned long flags, const void *data)
{
	(void)source; (void)target; (void)type; (void)flags; (void)data;
	return 0;
}
int test_mkdir(const char *path, mode_t mode)
{
	(void)path; (void)mode;
	return 0;
}
int test_chdir(const char *path) { (void)path; return 0; }
int test_reboot(int how) { (void)how; return 0; }
int test_poll(struct pollfd *fds, nfds_t n, int timeout)
{
	if (log_test) {
		assert(n == 1 && timeout == LOG_WAIT_MS);
		assert(++log_polls == 1);
		fds[0].revents = POLLOUT;
		return 1;
	}
	assert(timeout == 100);
	return poll(fds, n, timeout);
}
void cgroup_init(void) {}
void services_load(const char *dir)
{
	(void)dir;
	assert(pipe(readiness) == 0);
	assert(write(readiness[1], "\n", 1) == 1);
	assert(raise(SIGCHLD) == 0);
}
long long services_step(long long now, int stopping)
{
	(void)now; (void)stopping;
	if (steps++) {
		assert(ready_seen && child_seen);
		longjmp(done, 1);
	}
	return -1;
}
int services_pollfds(struct pollfd *pfd)
{
	*pfd = (struct pollfd){ .fd = readiness[0], .events = POLLIN };
	return 1;
}
void services_poll_events(const struct pollfd *pfd, int n)
{
	char byte;
	assert(n == 1 && (pfd[0].revents & POLLIN));
	assert(read(pfd[0].fd, &byte, 1) == 1);
	ready_seen = 1;
}
int reap_children(long long now)
{
	(void)now;
	assert(ready_seen);
	child_seen = 1;
	return 0;
}
int services_active(void) { return 0; }
int control_open(void) { assert(0); return -1; }
void control_handle(int fd) { (void)fd; assert(0); }

int main(void)
{
	sigset_t saved;
	/* A writable notification does not guarantee the next write succeeds. */
	log_test = 1;
	write_log("test", 4);
	assert(log_polls == 1 && log_writes == 2);
	write_log("test", 4); /* Stalled console: no further wait. */
	assert(log_polls == 1 && log_writes == 3);
	log_test = 2;
	log_writes = 0;
	write_log("test", 4); /* Successful output clears the stalled state. */
	log_test = 1;
	log_writes = log_polls = 0;
	write_log("test", 4);
	assert(log_polls == 1 && log_writes == 2);
	log_test = 0;
	assert(sigprocmask(SIG_SETMASK, NULL, &saved) == 0);
	if (!setjmp(done))
		vinit_main();
	assert(sigprocmask(SIG_SETMASK, &saved, NULL) == 0);
	close(readiness[0]);
	close(readiness[1]);
	puts("event fallback: ok");
	return 0;
}
