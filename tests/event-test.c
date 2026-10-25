/* Exercise PID 1's signalfd fallback without mounts or a PID namespace. */
#define _GNU_SOURCE
#include <poll.h>
static int test_poll(struct pollfd *, nfds_t, int);
#define main vinit_main
#define getpid test_getpid
#define signalfd test_signalfd
#define mount test_mount
#define mkdir test_mkdir
#define chdir test_chdir
#define reboot test_reboot
#define poll test_poll
#include "../src/vinit.c"
#undef main
#undef getpid
#undef signalfd
#undef mount
#undef mkdir
#undef chdir
#undef reboot
#undef poll
#include <assert.h>
#include <setjmp.h>

static jmp_buf done;
static int readiness[2], ready_seen, child_seen, steps;

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
	assert(timeout == 100);
	return poll(fds, n, timeout);
}
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
	assert(sigprocmask(SIG_SETMASK, NULL, &saved) == 0);
	if (!setjmp(done))
		vinit_main();
	assert(sigprocmask(SIG_SETMASK, &saved, NULL) == 0);
	close(readiness[0]);
	close(readiness[1]);
	puts("event fallback: ok");
	return 0;
}
