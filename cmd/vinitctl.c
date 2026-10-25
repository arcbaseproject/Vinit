/* vinitctl - control vinit through /run/vinit/control.
 *
 * Sends one request datagram and prints the reply. Exit status: 0 when
 * vinit replied "ok", 1 on an error reply, 2 on usage or transport errors.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_PATH "/run/vinit/control"

static int usage(void)
{
	fputs("usage: vinitctl status [service]\n"
	      "       vinitctl list\n"
	      "       vinitctl start|stop|restart service\n"
	      "       vinitctl reload|reboot|poweroff|halt\n", stderr);
	return 2;
}

int main(int argc, char **argv)
{
	struct sockaddr_un srv = { .sun_family = AF_UNIX };
	struct timeval tv = { .tv_sec = 5 };
	char req[256], reply[8193];
	sa_family_t autobind = AF_UNIX;
	ssize_t n;
	int fd;

	if (argc < 2 || argc > 3)
		return usage();
	if (snprintf(req, sizeof req, "%s%s%s", argv[1], argc == 3 ? " " : "",
		     argc == 3 ? argv[2] : "") >= (int)sizeof req) {
		fputs("vinitctl: request too long\n", stderr);
		return 2;
	}

	strcpy(srv.sun_path, CONTROL_PATH);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	/* Binding to just the family autobinds a unique abstract address, so
	 * vinit has somewhere to send the reply. */
	if (fd < 0 || bind(fd, (struct sockaddr *)&autobind, sizeof autobind) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0 ||
	    connect(fd, (struct sockaddr *)&srv, sizeof srv) < 0) {
		fprintf(stderr, "vinitctl: %s: %s\n", CONTROL_PATH, strerror(errno));
		return 2;
	}
	if (send(fd, req, strlen(req), 0) < 0) {
		fprintf(stderr, "vinitctl: send: %s\n", strerror(errno));
		return 2;
	}
	n = recv(fd, reply, sizeof reply - 1, 0);
	if (n < 0) {
		fprintf(stderr, "vinitctl: no reply: %s\n",
			errno == EAGAIN ? "timed out" : strerror(errno));
		return 2;
	}
	reply[n] = '\0';

	if (strncmp(reply, "ok\n", 3) == 0) {
		fputs(reply + 3, stdout);
		return 0;
	}
	fprintf(stderr, "vinitctl: %s", strncmp(reply, "error: ", 7) == 0 ? reply + 7 : reply);
	return 1;
}
