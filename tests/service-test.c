/* Supervision regressions; runs without PID 1 or elevated privileges. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
	fputs("[service]\ncommand = /bin/sh -c true\ntype = oneshot\n", f);
	assert(fclose(f) == 0);
	memset(svc, 0, sizeof svc);
	services_load(dir);
	assert(nsvc == 1 && svc[0].type == TYPE_ONESHOT);
	svc[0].state = ST_RUNNING; /* A successfully completed oneshot. */
	f = fopen(path, "w");
	assert(f);
	fputs("[service]\ncommand = /bin/sh -c true\ntype = simple\n", f);
	assert(fclose(f) == 0);
	assert(services_reload(dir, 1000, msg, sizeof msg) == 0);
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

static pid_t sleeper(void)
{
	pid_t pid = fork();

	assert(pid >= 0);
	if (!pid) {
		setsid();
		for (;;)
			pause();
	}
	return pid;
}

static void reap_until_gone(struct service *s)
{
	for (int i = 0; s->pid && i < 1000; i++) {
		reap_children(0);
		usleep(1000);
	}
	assert(!s->pid);
}

/* A required service that crashes takes its dependents down with it. They
 * start again once it is back, and fail if it fails. */
static void requirement_goes_down(void)
{
	memset(svc, 0, sizeof svc);
	nsvc = 2;
	strcpy(svc[0].name, "base");
	strcpy(svc[1].name, "app");
	strcpy(svc[1].deps[0].name, "base");
	svc[1].deps[0].hard = 1;
	svc[1].ndeps = 1;
	config_resolve(svc, nsvc);
	for (int i = 0; i < nsvc; i++) {
		svc[i].pid = sleeper();
		svc[i].state = ST_RUNNING;
		svc[i].want = 1;
		svc[i].stop_timeout_ms = 1000;
	}
	svc[0].restart = RESTART_ON_FAILURE;
	svc[0].delay_ms = 1000;

	kill(svc[0].pid, SIGKILL);
	reap_until_gone(&svc[0]);
	assert(svc[0].state == ST_INACTIVE && svc[0].want);
	assert(svc[1].state == ST_STOPPING && svc[1].restarting);
	reap_until_gone(&svc[1]);
	assert(svc[1].state == ST_INACTIVE && svc[1].want);
	services_step(500, 0); /* base restarts at 1000; app waits for it */
	assert(!svc[1].pid && svc[1].state == ST_INACTIVE);

	svc[0].state = ST_FAILED; /* base gives up */
	svc[0].want = 0;
	services_step(500, 0);
	assert(svc[1].state == ST_FAILED);
}

/* Whoever can write the services directory could add root services. */
static void unsafe_service_dir(void)
{
	char dir[] = "/tmp/vinit-service-test.XXXXXX";
	char path[256];
	int fd;
	FILE *f;

	assert(mkdtemp(dir));
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	assert(fd >= 0);
	snprintf(path, sizeof path, "%s/job.conf", dir);
	f = fopen(path, "w");
	assert(f);
	fputs("[service]\ncommand = /bin/sh -c true\n", f);
	assert(fclose(f) == 0);
	assert(fchmod(fd, 0777) == 0);
	memset(svc, 0, sizeof svc);
	services_load(dir);
	assert(nsvc == 1 && svc[0].builtin);
	assert(fchmod(fd, 0755) == 0);
	assert(close(fd) == 0);
	memset(svc, 0, sizeof svc);
	services_load(dir);
	assert(nsvc == 1 && !svc[0].builtin);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
}

static void put(const char *dir, const char *name, const char *text)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	assert(fclose(f) == 0);
}

/* A successful oneshot repeats only with policy = always, using backoff. */
static void oneshot_restart_always(void)
{
	char dir[] = "/tmp/vinit-service-test.XXXXXX";
	char path[256];

	assert(mkdtemp(dir));
	put(dir, "job.conf", "[service]\ncommand = /bin/sh -c true\ntype = oneshot\n"
	    "[restart]\npolicy = always\ndelay_ms = 100\nlimit = 1\n");
	memset(svc, 0, sizeof svc);
	services_load(dir);
	services_step(0, 0);
	reap_until_gone(&svc[0]);
	assert(svc[0].state == ST_INACTIVE && svc[0].want);
	assert(svc[0].failures == 1 && svc[0].deadline == 100);
	services_step(99, 0);
	assert(!svc[0].pid);
	services_step(100, 0);
	assert(svc[0].pid);
	reap_until_gone(&svc[0]);
	assert(svc[0].state == ST_FAILED && !svc[0].want);
	snprintf(path, sizeof path, "%s/job.conf", dir);
	assert(unlink(path) == 0);
	assert(rmdir(dir) == 0);
}

/* A changed running service restarts on reload; an unchanged one does not. */
static void reload_restarts_changed(void)
{
	char dir[] = "/tmp/vinit-service-test.XXXXXX";
	char msg[256], cmd[64];

	assert(mkdtemp(dir));
	put(dir, "a.conf", "[service]\ncommand = /bin/sh -c \"exec sleep 100\"\n");
	put(dir, "b.conf", "[service]\ncommand = /bin/sh -c \"exec sleep 100\"\n");
	memset(svc, 0, sizeof svc);
	services_load(dir);
	services_step(0, 0);
	assert(svc[0].state == ST_RUNNING && svc[1].state == ST_RUNNING);
	put(dir, "b.conf", "[service]\ncommand = /bin/sh -c \"exec sleep 200\"\n");
	assert(services_reload(dir, 0, msg, sizeof msg) == 0);
	assert(strcmp(msg, "0 added, 0 removed, 1 restarted") == 0);
	assert(svc[0].state == ST_RUNNING);
	assert(svc[1].state == ST_STOPPING && svc[1].restarting);
	reap_until_gone(&svc[1]);
	services_step(0, 0);
	assert(svc[1].state == ST_RUNNING && svc[1].pid);
	service_stop(0);
	service_stop(1);
	services_step(0, 0);
	reap_until_gone(&svc[0]);
	reap_until_gone(&svc[1]);
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	(void)!system(cmd);
}

/* Output of a service with log = X reaches X's stdin. */
static void log_pipe(void)
{
	char dir[] = "/tmp/vinit-service-test.XXXXXX";
	char text[256], out[256] = "", cmd[64];
	FILE *f;

	assert(mkdtemp(dir));
	put(dir, "app.conf", "[service]\ncommand = /bin/sh -c \"echo hello; echo world >&2\"\n"
	    "type = oneshot\n[execution]\nlog = logger\n");
	snprintf(text, sizeof text, "[service]\ncommand = /bin/sh -c \"head -n2 > %s/out\"\n", dir);
	put(dir, "logger.conf", text);
	memset(svc, 0, sizeof svc);
	services_load(dir);
	for (int i = 0; i < 1000 && (svc[0].state != ST_RUNNING || svc[1].want); i++) {
		services_step(0, 0);
		reap_children(0);
		usleep(1000);
	}
	assert(svc[0].state == ST_RUNNING && svc[1].state == ST_INACTIVE);
	snprintf(text, sizeof text, "%s/out", dir);
	f = fopen(text, "r");
	assert(f);
	assert(fread(out, 1, sizeof out - 1, f) == 12);
	fclose(f);
	assert(strcmp(out, "hello\nworld\n") == 0);
	close(svc[1].logpipe[0]);
	close(svc[1].logpipe[1]);
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	(void)!system(cmd);
}

int main(void)
{
	shared_dependencies();
	readiness_eof();
	reload_completed_oneshot();
	requirement_goes_down();
	unsafe_service_dir();
	oneshot_restart_always();
	reload_restarts_changed();
	log_pipe();
	puts("service test: PASS");
	return 0;
}
