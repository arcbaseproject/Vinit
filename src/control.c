/* Control socket: /run/vinit/control, a Unix datagram socket.
 *
 * One request per datagram, one reply datagram to the sender's address.
 * Datagrams keep PID 1 free of per-client state: a client that hangs or
 * disappears costs nothing. The kernel attaches each sender's real
 * credentials (SO_PASSCRED), which decide what the sender may do.
 *
 * Request: "<command> [<service>]", printable ASCII, at most 255 bytes.
 * Reply:   "ok\n" followed by output, or "error: <message>\n".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vinit.h"

#define CONTROL_DIR   "/run/vinit"
#define CONTROL_PATH  CONTROL_DIR "/control"
#define MAX_REQUEST   255
#define MAX_PER_WAKE  16 /* bounds the work one wakeup can cause */

static const char *const state_names[] = {
	[ST_INACTIVE] = "inactive",
	[ST_STARTING] = "starting",
	[ST_RUNNING]  = "running",
	[ST_STOPPING] = "stopping",
	[ST_FAILED]   = "failed",
};

struct reply {
	char buf[8192];
	size_t len;
};

static void add(struct reply *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(struct reply *r, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(r->buf + r->len, sizeof r->buf - r->len, fmt, ap);
	va_end(ap);
	if (n > 0)
		r->len += (size_t)n < sizeof r->buf - r->len ? (size_t)n : sizeof r->buf - r->len - 1;
}

int control_open(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	struct stat st;
	int fd, on = 1;

	if (mkdir(CONTROL_DIR, 0755) < 0 && errno != EEXIST) {
		warn("mkdir %s: %s", CONTROL_DIR, strerror(errno));
		return -1;
	}
	/* Only trust a directory nobody else could have prepared. */
	if (lstat(CONTROL_DIR, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
	    (st.st_mode & 022)) {
		warn("%s is not a root-owned directory; no control socket", CONTROL_DIR);
		return -1;
	}
	unlink(CONTROL_PATH);
	strcpy(a.sun_path, CONTROL_PATH);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof on) < 0 ||
	    bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ||
	    /* Anyone may connect; permissions are checked per request. */
	    chmod(CONTROL_PATH, 0666) < 0) {
		warn("control socket: %s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

static void describe_exit(struct reply *r, const struct service *s)
{
	if (!s->exited)
		add(r, "none\n");
	else if (WIFEXITED(s->last_status))
		add(r, "status %d\n", WEXITSTATUS(s->last_status));
	else
		add(r, "signal %d (%s)\n", WTERMSIG(s->last_status), strsignal(WTERMSIG(s->last_status)));
}

static void cmd_status(struct reply *r, int i)
{
	const struct service *s;
	int count[ST_FAILED + 1] = { 0 };

	if (i < 0) {
		for (int j = 0; j < nsvc; j++)
			count[svc[j].state]++;
		add(r, "ok\n%s, %d services: %d running, %d starting, %d stopping, %d inactive, %d failed\n",
		    shutdown_how ? "shutting down" : "up", nsvc, count[ST_RUNNING], count[ST_STARTING],
		    count[ST_STOPPING], count[ST_INACTIVE], count[ST_FAILED]);
		return;
	}
	s = &svc[i];
	add(r, "ok\nname: %s\nstate: %s\n", s->name, state_names[s->state]);
	if (s->pid)
		add(r, "pid: %d\n", (int)s->pid);
	add(r, "type: %s\nrestarts: %d\nlast exit: ", s->type == TYPE_ONESHOT ? "oneshot" : "simple",
	    s->failures);
	describe_exit(r, s);
	if (s->error[0])
		add(r, "error: %s\n", s->error);
}

static void cmd_list(struct reply *r)
{
	add(r, "ok\n%-*s %-9s %s\n", NAME_LEN - 1, "SERVICE", "STATE", "PID");
	for (int i = 0; i < nsvc; i++) {
		add(r, "%-*s %-9s ", NAME_LEN - 1, svc[i].name, state_names[svc[i].state]);
		if (svc[i].pid)
			add(r, "%d\n", (int)svc[i].pid);
		else
			add(r, "-\n");
	}
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

enum { ARG_NONE, ARG_OPTIONAL, ARG_REQUIRED };
enum { C_STATUS, C_LIST, C_START, C_STOP, C_RESTART, C_RELOAD, C_REBOOT, C_POWEROFF, C_HALT };

static const struct {
	const char *name;
	int arg;
	int privileged;
} commands[] = {
	[C_STATUS]   = { "status",   ARG_OPTIONAL, 0 },
	[C_LIST]     = { "list",     ARG_NONE,     0 },
	[C_START]    = { "start",    ARG_REQUIRED, 1 },
	[C_STOP]     = { "stop",     ARG_REQUIRED, 1 },
	[C_RESTART]  = { "restart",  ARG_REQUIRED, 1 },
	[C_RELOAD]   = { "reload",   ARG_NONE,     1 },
	[C_REBOOT]   = { "reboot",   ARG_NONE,     1 },
	[C_POWEROFF] = { "poweroff", ARG_NONE,     1 },
	[C_HALT]     = { "halt",     ARG_NONE,     1 },
};

static void handle_request(struct reply *r, char *req, const struct ucred *cred)
{
	const char *err = NULL;
	char *verb, *arg, *extra;
	size_t c;
	int i = -1;

	for (char *p = req; *p; p++)
		if (*p < 0x20 || *p > 0x7e) {
			add(r, "error: malformed request\n");
			return;
		}
	verb = strtok(req, " ");
	arg = strtok(NULL, " ");
	extra = strtok(NULL, " ");
	if (!verb) {
		add(r, "error: empty request\n");
		return;
	}
	for (c = 0; c < sizeof commands / sizeof commands[0] && strcmp(commands[c].name, verb); c++)
		;
	if (c == sizeof commands / sizeof commands[0]) {
		add(r, "error: unknown command '%.32s'\n", verb);
		return;
	}
	if (extra || (arg && commands[c].arg == ARG_NONE) || (!arg && commands[c].arg == ARG_REQUIRED)) {
		add(r, "error: usage: %s%s\n", commands[c].name,
		    commands[c].arg == ARG_NONE ? "" :
		    commands[c].arg == ARG_OPTIONAL ? " [service]" : " service");
		return;
	}
	if (commands[c].privileged && cred->uid != 0) {
		add(r, "error: permission denied\n");
		return;
	}
	if (arg && (i = service_find(arg)) < 0) {
		add(r, "error: unknown service '%.32s'\n", arg);
		return;
	}
	if (shutdown_how && commands[c].privileged && c != C_STOP) {
		add(r, "error: shutting down\n");
		return;
	}

	switch (c) {
	case C_STATUS:
		cmd_status(r, i);
		return;
	case C_LIST:
		cmd_list(r);
		return;
	case C_START:
		err = service_start(i);
		break;
	case C_STOP:
		service_stop(i);
		break;
	case C_RESTART:
		err = service_restart(i, now_ms());
		break;
	case C_RELOAD: {
		static char msg[256];

		if (services_reload(SERVICE_DIR, now_ms(), msg, sizeof msg) < 0)
			err = msg;
		else
			add(r, "ok\n%s\n", msg);
		break;
	}
	case C_REBOOT:
		request_shutdown(RB_AUTOBOOT);
		break;
	case C_POWEROFF:
		request_shutdown(RB_POWER_OFF);
		break;
	case C_HALT:
		request_shutdown(RB_HALT_SYSTEM);
		break;
	}
	if (err)
		add(r, "error: %s\n", err);
	else if (c != C_RELOAD)
		add(r, "ok\n");
	if (commands[c].privileged)
		say("%s%s%s requested by uid %d (pid %d)", verb, arg ? " " : "", arg ? arg : "",
		    (int)cred->uid, (int)cred->pid);
}

void control_handle(int fd)
{
	for (int n = 0; n < MAX_PER_WAKE; n++) {
		union {
			struct cmsghdr align;
			char buf[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(16 * sizeof(int))];
		} cm;
		char req[MAX_REQUEST + 1];
		struct sockaddr_un from;
		struct iovec iov = { req, MAX_REQUEST };
		struct msghdr mh = {
			.msg_name = &from, .msg_namelen = sizeof from,
			.msg_iov = &iov, .msg_iovlen = 1,
			.msg_control = cm.buf, .msg_controllen = sizeof cm.buf,
		};
		const struct ucred *cred = NULL;
		struct reply r;
		ssize_t len;

		len = recvmsg(fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (len < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				warn("control: recvmsg: %s", strerror(errno));
			return;
		}

		/* Close any file descriptors the sender passed. */
		for (struct cmsghdr *h = CMSG_FIRSTHDR(&mh); h; h = CMSG_NXTHDR(&mh, h)) {
			if (h->cmsg_level != SOL_SOCKET)
				continue;
			if (h->cmsg_type == SCM_CREDENTIALS)
				cred = (const struct ucred *)CMSG_DATA(h);
			else if (h->cmsg_type == SCM_RIGHTS)
				for (size_t k = 0; k < (h->cmsg_len - CMSG_LEN(0)) / sizeof(int); k++) {
					int passed;

					memcpy(&passed, CMSG_DATA(h) + k * sizeof(int), sizeof passed);
					close(passed);
				}
		}

		r.len = 0;
		req[len] = '\0';
		if (mh.msg_flags & MSG_TRUNC)
			add(&r, "error: request too long\n");
		else if (!cred)
			add(&r, "error: no credentials\n");
		else
			handle_request(&r, req, cred);

		/* An unbound sender cannot get a reply; a gone one gets none. */
		if (mh.msg_namelen > sizeof(sa_family_t))
			sendto(fd, r.buf, r.len, MSG_DONTWAIT | MSG_NOSIGNAL,
			       (struct sockaddr *)&from, mh.msg_namelen);
	}
}
