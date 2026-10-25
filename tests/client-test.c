/* Check both transport deadlines before exercising a timed-out send. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
static int test_socket(int, int, int);
static int test_bind(int, const struct sockaddr *, socklen_t);
static int test_connect(int, const struct sockaddr *, socklen_t);
static int test_setsockopt(int, int, int, const void *, socklen_t);
static ssize_t test_send(int, const void *, size_t, int);
static ssize_t test_recv(int, void *, size_t, int);
#define main vinitctl_main
#define socket test_socket
#define bind test_bind
#define connect test_connect
#define setsockopt test_setsockopt
#define send test_send
#define recv test_recv
#include "../cmd/vinitctl.c"
#undef main

static int receive_timeout, send_timeout;
static int test_socket(int domain, int type, int protocol)
{
	(void)domain; (void)type; (void)protocol;
	return 42;
}
static int test_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	(void)fd; (void)addr; (void)len;
	return 0;
}
static int test_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	(void)fd; (void)addr; (void)len;
	return 0;
}
static int test_setsockopt(int fd, int level, int option,
                           const void *value, socklen_t len)
{
	const struct timeval *tv = value;
	assert(fd == 42 && level == SOL_SOCKET && len == sizeof *tv);
	assert(tv->tv_sec == 5 && tv->tv_usec == 0);
	if (option == SO_RCVTIMEO)
		receive_timeout = 1;
	else {
		assert(option == SO_SNDTIMEO);
		send_timeout = 1;
	}
	return 0;
}
static ssize_t test_send(int fd, const void *buf, size_t len, int flags)
{
	(void)fd; (void)buf; (void)len; (void)flags;
	assert(receive_timeout && send_timeout);
	errno = EAGAIN;
	return -1;
}
static ssize_t test_recv(int fd, void *buf, size_t len, int flags)
{
	(void)fd; (void)buf; (void)len; (void)flags;
	assert(0);
	return -1;
}
int main(void)
{
	char *args[] = { "vinitctl", "status", NULL };
	assert(vinitctl_main(2, args) == 2);
	puts("client send timeout: ok");
	return 0;
}
