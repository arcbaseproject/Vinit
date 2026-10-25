/* Supervision regressions; runs without PID 1 or elevated privileges. */
#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/vinit.h"

void say(const char *fmt, ...) { (void)fmt; }
void warn(const char *fmt, ...) { (void)fmt; }

static void shared_dependencies(void)
{
	memset(svc, 0, sizeof svc);
	nsvc = MAX_SERVICES - 1;
	for (int i = 0; i < nsvc; i++) {
		snprintf(svc[i].name, NAME_LEN, "s%d", i);
		for (int j = i - 1; j >= 0 && j >= i - 2; j--) {
			struct dep *d = &svc[i].deps[svc[i].ndeps++];

			snprintf(d->name, NAME_LEN, "s%d", j);
			d->idx = -1;
			d->hard = 1;
		}
	}
	config_resolve(svc, nsvc);
	for (int i = 0; i < nsvc; i++)
		assert(!svc[i].error[0]);
	/* The old traversal takes exponential time even on the second start. */
	alarm(3);
	assert(service_start(nsvc - 1) == NULL);
	assert(service_start(nsvc - 1) == NULL);
	alarm(0);
	for (int i = 0; i < nsvc; i++)
		assert(svc[i].want);
	strcpy(svc[0].error, "broken dependency");
	assert(service_start(nsvc - 1) == svc[0].error);
}

static void readiness_eof(void)
{
	int p[2];
	pid_t pid;
	struct pollfd fd;
	char ready;

	memset(svc, 0, sizeof svc);
	nsvc = 1;
	assert(pipe(p) == 0);
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		close(p[0]);
		assert(setsid() >= 0);
		assert(write(p[1], "x", 1) == 1);
		close(p[1]);
		for (;;)
			pause();
	}
	close(p[1]);
	assert(read(p[0], &ready, 1) == 1);
	assert(read(p[0], &ready, 1) == 0);
	svc[0].state = ST_STARTING;
	svc[0].want = 1;
	svc[0].pid = pid;
	svc[0].notify = p[0];
	svc[0].ready_fd = 3;
	svc[0].ready_timeout_ms = 100;
	svc[0].stop_timeout_ms = 100;
	svc[0].deadline = 100;
	fd = (struct pollfd){ .fd = p[0], .revents = POLLHUP };
	services_poll_events(&fd, 1);
	assert(!svc[0].notify);
	assert(services_step(50, 0) == 50);
	services_step(100, 0);
	assert(svc[0].state == ST_STOPPING && svc[0].timed_out);
	for (int i = 0; svc[0].pid && i < 1000; i++) {
		reap_children(101);
		usleep(1000);
	}
	if (svc[0].pid) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		assert(!"readiness timeout did not terminate child");
	}
	assert(svc[0].state == ST_FAILED);
}

static void reload_completed_oneshot(void)
{
	char dir[] = "/tmp/vinit-service-test.XXXXXX";
	char path[256], msg[256];
	FILE *f;

	assert(mkdtemp(dir));
	snprintf(path, sizeof path, "%s/job.conf", dir);
	f = fopen(path, "w");
	assert(f);
	fputs("[service]\ncommand = /bin/true\ntype = oneshot\n", f);
	assert(fclose(f) == 0);
	memset(svc, 0, sizeof svc);
	services_load(dir);
	assert(nsvc == 1 && svc[0].type == TYPE_ONESHOT);
	svc[0].state = ST_RUNNING; /* A successfully completed oneshot. */
	f = fopen(path, "w");
	assert(f);
	fputs("[service]\ncommand = /bin/true\ntype = simple\n", f);
	assert(fclose(f) == 0);
	assert(services_reload(dir, msg, sizeof msg) == 0);
	assert(svc[0].type == TYPE_SIMPLE);
	assert(service_restart(0, 1000) == NULL);
	assert(svc[0].state == ST_INACTIVE && svc[0].want);
	services_step(1000, 0);
	assert(svc[0].pid > 0);
	for (int i = 0; svc[0].pid && i < 1000; i++) {
		reap_children(1001);
		usleep(1000);
	}
	assert(!svc[0].pid && svc[0].state == ST_INACTIVE);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
}

int main(void)
{
	shared_dependencies();
	readiness_eof();
	reload_completed_oneshot();
	puts("service test: PASS");
	return 0;
}
