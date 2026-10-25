/* Service supervision.
 *
 * Every decision is made in services_step(), which the main loop calls
 * after each event: start services whose dependencies are ready, stop
 * services that are no longer wanted once nothing depends on them, and
 * enforce stop timeouts. reap_children() records exits and applies the
 * restart policy.
 *
 * Each service runs in its own session, so its PID is also its process
 * group ID, and stopping it signals the whole group.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vinit.h"

#define HEALTHY_MS      10000 /* a run this long resets the failure count */
#define MAX_BACKOFF_MS  60000
#define MAX_GROUPS      32

struct creds {
	int drop;
	uid_t uid;
	gid_t gid;
	gid_t groups[MAX_GROUPS];
	int ngroups;
	char login[NAME_LEN];
};

/* Sent from the child over a close-on-exec pipe when setup or exec fails.
 * The string literal is valid in the parent too: fork() copied it. */
struct child_error {
	const char *what;
	int err;
};

struct service svc[MAX_SERVICES];
int nsvc;

static int cmp_names(const void *a, const void *b)
{
	return strcmp(a, b);
}

/* Parse and resolve every service file in dir into tab. At most
 * MAX_SERVICES - 1 entries, so one slot stays free for the emergency
 * shell. */
static int load_table(const char *dir, struct service *tab)
{
	static char names[MAX_SERVICES - 1][NAME_LEN + 5];
	char path[512];
	struct dirent *e;
	struct stat st;
	int n = 0;
	DIR *d;

	/* Whoever can write the directory can add services that run as root. */
	if (stat(dir, &st) == 0 && (st.st_uid != geteuid() || (st.st_mode & 022))) {
		warn("ignoring %s: must be owned by uid %d and not writable by group or others",
		     dir, (int)geteuid());
		return 0;
	}

	if ((d = opendir(dir))) {
		while ((e = readdir(d))) {
			size_t len = strlen(e->d_name);

			if (e->d_name[0] == '.' || len <= 5 || strcmp(e->d_name + len - 5, ".conf"))
				continue;
			if (len >= sizeof names[0])
				warn("ignoring %s: name too long", e->d_name);
			else if (n == MAX_SERVICES - 1)
				warn("ignoring %s: too many services", e->d_name);
			else
				memcpy(names[n++], e->d_name, len + 1);
		}
		closedir(d);
	} else if (errno != ENOENT) {
		warn("open %s: %s", dir, strerror(errno));
	}

	/* Sorted, so that start order among independent services is stable. */
	qsort(names, (size_t)n, sizeof names[0], cmp_names);
	for (int i = 0; i < n; i++) {
		if (snprintf(path, sizeof path, "%s/%s", dir, names[i]) >= (int)sizeof path)
			snprintf(tab[i].error, sizeof tab[i].error, "path too long");
		else
			config_parse(path, &tab[i], tab[i].error, sizeof tab[i].error);
	}
	config_resolve(tab, n);
	return n;
}

/* Add /bin/sh on the console, unless it exists. Returns 1 if added. The
 * table always has a free slot: load_table() fills at most
 * MAX_SERVICES - 1. */
static int emergency_shell(void)
{
	struct service *s;

	for (int i = 0; i < nsvc; i++)
		if (svc[i].builtin)
			return 0;
	if (nsvc == MAX_SERVICES)
		return 0;
	s = &svc[nsvc++];
	memset(s, 0, sizeof *s);
	strcpy(s->name, "emergency-shell");
	strcpy(s->cmdbuf, "/bin/sh");
	s->argv[0] = s->cmdbuf;
	s->restart = RESTART_ALWAYS;
	s->delay_ms = 1000;
	s->stop_timeout_ms = 5000;
	s->stdin_console = 1;
	s->builtin = 1;
	s->want = 1;
	return 1;
}

void services_load(const char *dir)
{
	int ok = 0;

	nsvc = load_table(dir, svc);
	for (int i = 0; i < nsvc; i++) {
		if (svc[i].error[0]) {
			svc[i].state = ST_FAILED;
			warn("service %s failed: %s", svc[i].name, svc[i].error);
		} else {
			svc[i].want = 1;
			ok++;
		}
	}

	/* A system without usable services still gets a shell. */
	if (!ok) {
		warn("no usable services in %s, starting /bin/sh", dir);
		emergency_shell();
	}
}

static int parse_id(const char *s, unsigned *out)
{
	unsigned long v;
	char *end;

	if (!isdigit((unsigned char)s[0]))
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v >= UINT_MAX)
		return -1;
	*out = (unsigned)v;
	return 0;
}

/* Read /etc/passwd and /etc/group directly with fgetpwent()/fgetgrent().
 * getpwnam() could load NSS modules into PID 1, and in a static binary
 * it may not work at all. */
static int lookup_creds(const struct service *s, struct creds *c)
{
	struct passwd *pw;
	struct group *gr;
	unsigned id = 0;
	int numeric, found = 0;
	FILE *f;

	memset(c, 0, sizeof *c);
	c->drop = s->user[0] || s->group[0];
	if (!c->drop)
		return 0;

	if (s->user[0]) {
		numeric = parse_id(s->user, &id) == 0;
		if ((f = fopen("/etc/passwd", "re"))) {
			while (!found && (pw = fgetpwent(f))) {
				if (numeric ? pw->pw_uid == id : strcmp(pw->pw_name, s->user) == 0) {
					c->uid = pw->pw_uid;
					c->gid = pw->pw_gid;
					snprintf(c->login, sizeof c->login, "%s", pw->pw_name);
					found = 1;
				}
			}
			fclose(f);
		}
		if (!found && !numeric) {
			warn("%s: unknown user '%s'", s->name, s->user);
			return -1;
		}
		if (!found)
			c->uid = c->gid = id;
	}

	if (s->group[0]) {
		numeric = parse_id(s->group, &id) == 0;
		found = 0;
		if ((f = fopen("/etc/group", "re"))) {
			while (!found && (gr = fgetgrent(f))) {
				if (numeric ? gr->gr_gid == id : strcmp(gr->gr_name, s->group) == 0) {
					c->gid = gr->gr_gid;
					found = 1;
				}
			}
			fclose(f);
		}
		if (!found && !numeric) {
			warn("%s: unknown group '%s'", s->name, s->group);
			return -1;
		}
		if (!found)
			c->gid = id;
	}

	c->groups[c->ngroups++] = c->gid;
	if (c->login[0] && (f = fopen("/etc/group", "re"))) {
		while (c->ngroups < MAX_GROUPS && (gr = fgetgrent(f)))
			for (char **m = gr->gr_mem; *m; m++)
				if (strcmp(*m, c->login) == 0 && gr->gr_gid != c->gid) {
					c->groups[c->ngroups++] = gr->gr_gid;
					break;
				}
		fclose(f);
	}
	return 0;
}

static _Noreturn void child(const struct service *s, const struct creds *c, int errfd, int readyfd)
{
	struct child_error ce;
	sigset_t none;
	int fd, p[2];

	/* The signal mask survives execve(); services must not inherit ours. */
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	setsid();

	if (s->ready_fd) {
		/* Move the error pipe above ready_fd so dup2() cannot hit it. */
		fd = fcntl(errfd, F_DUPFD_CLOEXEC, s->ready_fd + 1);
		if (fd >= 0)
			errfd = fd;
		ce.what = "ready_fd";
		if (readyfd == s->ready_fd ? fcntl(readyfd, F_SETFD, 0) < 0 :
		    dup2(readyfd, s->ready_fd) < 0)
			goto fail;
	}

	if (!s->stdin_console) {
		/* Without /dev/null (no devtmpfs), a pipe with no writer also
		 * reads as EOF. */
		fd = open("/dev/null", O_RDONLY);
		if (fd < 0 && pipe(p) == 0) {
			close(p[1]);
			fd = p[0];
		}
		if (fd > 0) {
			dup2(fd, 0);
			close(fd);
		}
	}

	if (c->drop) {
		ce.what = "setgroups";
		if (setgroups((size_t)c->ngroups, c->groups) < 0)
			goto fail;
		ce.what = "setgid";
		if (setgid(c->gid) < 0)
			goto fail;
		ce.what = "setuid";
		if (s->user[0] && setuid(c->uid) < 0)
			goto fail;
	}
	/* After dropping privileges, so the service user needs access. */
	ce.what = "chdir";
	if (chdir(s->workdir[0] ? s->workdir : "/") < 0)
		goto fail;
	for (int i = 0; i < s->nenv; i++)
		putenv((char *)s->env[i]);

	ce.what = "exec";
	execv(s->argv[0], s->argv);
fail:
	ce.err = errno;
	(void)!write(errfd, &ce, sizeof ce);
	_exit(127);
}

static void finished(struct service *s, int ok, long long now);

static void start_service(struct service *s, long long now)
{
	struct child_error ce;
	struct creds c;
	int pfd[2], rp[2] = { -1, -1 };
	ssize_t r;
	pid_t pid;

	s->started_at = now;
	s->state = ST_STARTING;
	if (lookup_creds(s, &c) < 0) {
		finished(s, 0, now);
		return;
	}
	if (s->ready_fd && pipe2(rp, O_CLOEXEC) < 0)
		goto fail;
	if (pipe2(pfd, O_CLOEXEC) < 0)
		goto fail;
	if ((pid = fork()) < 0) {
		close(pfd[0]);
		close(pfd[1]);
		goto fail;
	}
	if (pid == 0)
		child(s, &c, pfd[1], rp[1]);
	close(pfd[1]);
	if (s->ready_fd) {
		close(rp[1]);
		fcntl(rp[0], F_SETFL, O_NONBLOCK);
		s->notify = rp[0];
	}
	/* Blocks only until the child calls execv() or fails before it.
	 * EOF means the exec succeeded. Signals are blocked: no EINTR. */
	r = read(pfd[0], &ce, sizeof ce);
	close(pfd[0]);
	s->pid = pid;

	if (r == (ssize_t)sizeof ce) {
		warn("%s: %s %s: %s", s->name, ce.what, s->argv[0], strerror(ce.err));
		return; /* the child exits with 127 and gets reaped */
	}
	if (s->type == TYPE_SIMPLE && !s->ready_fd)
		s->state = ST_RUNNING;
	if (s->ready_fd)
		s->deadline = s->ready_timeout_ms ? now + s->ready_timeout_ms : 0;
	say("started %s (pid %d)", s->name, (int)pid);
	return;
fail:
	warn("%s: cannot start: %s", s->name, strerror(errno));
	if (rp[0] >= 0) {
		close(rp[0]);
		close(rp[1]);
	}
	finished(s, 0, now);
}

static void stop_service(struct service *s, long long now)
{
	if (!s->pid) {
		/* Finished oneshot, or waiting to restart. */
		s->state = ST_INACTIVE;
		return;
	}
	say("stopping %s", s->name);
	kill(-s->pid, SIGTERM);
	kill(-s->pid, SIGCONT);
	s->state = ST_STOPPING;
	s->deadline = now + s->stop_timeout_ms;
}

/* The process ended, or could not be started. Apply the restart policy. */
static void finished(struct service *s, int ok, long long now)
{
	long long delay;

	s->pid = 0;
	if (s->notify) {
		close(s->notify);
		s->notify = 0;
	}
	if (s->timed_out) {
		/* Stopped for not becoming ready: that is a failure. */
		s->timed_out = 0;
		s->state = ST_STARTING;
		ok = 0;
	}
	if (s->state == ST_STOPPING || !s->want) {
		s->state = ST_INACTIVE;
		if (s->restarting && s->want) {
			s->restarting = 0;
			s->failures = 0;
			s->deadline = 0;
			say("stopped %s, starting it again", s->name);
			return;
		}
		s->want = 0;
		say("stopped %s", s->name);
		return;
	}
	if (s->type == TYPE_ONESHOT && ok) {
		s->state = ST_RUNNING; /* done; dependents may start */
		s->failures = 0;
		say("finished %s", s->name);
		return;
	}
	if (s->restart == RESTART_ALWAYS || (s->restart == RESTART_ON_FAILURE && !ok)) {
		if (now - s->started_at >= HEALTHY_MS)
			s->failures = 0;
		if (s->limit && s->failures >= s->limit) {
			s->state = ST_FAILED;
			s->want = 0;
			warn("%s failed: restart limit (%d) reached", s->name, s->limit);
			return;
		}
		/* Exponential backoff, capped at MAX_BACKOFF_MS (or delay_ms if
		 * that is larger). Only the first restart may be immediate, so
		 * delay_ms = 0 cannot spin PID 1. */
		delay = s->failures == 0 ? s->delay_ms :
			(long long)(s->delay_ms > 0 ? s->delay_ms : 100) << (s->failures < 16 ? s->failures : 16);
		if (delay > MAX_BACKOFF_MS)
			delay = s->delay_ms > MAX_BACKOFF_MS ? s->delay_ms : MAX_BACKOFF_MS;
		s->failures++;
		s->state = ST_INACTIVE;
		s->deadline = now + delay;
		say("restarting %s in %lld ms", s->name, delay);
		return;
	}
	s->state = ok ? ST_INACTIVE : ST_FAILED;
	s->want = 0;
	if (!ok)
		warn("%s failed", s->name);
}

int reap_children(long long now)
{
	for (;;) {
		siginfo_t si;
		struct service *s = NULL;
		int st;

		/* Peek with WNOWAIT first: while the zombie exists, its PID (and
		 * so its process group ID) cannot be reused, which makes the
		 * group kill below safe. */
		si.si_pid = 0;
		if (waitid(P_ALL, 0, &si, WEXITED | WNOHANG | WNOWAIT) < 0)
			return 0; /* ECHILD: no children left */
		if (si.si_pid == 0)
			return 1;

		for (int i = 0; i < nsvc; i++)
			if (svc[i].pid == si.si_pid)
				s = &svc[i];
		/* A simple service's leftover processes die with it. A oneshot
		 * may leave background processes on purpose. */
		if (s && (s->type == TYPE_SIMPLE || s->state == ST_STOPPING))
			kill(-s->pid, SIGKILL);
		/* Cannot fail for a child waitid() just reported; if it did,
		 * peeking again would loop on the same child forever. */
		if (waitpid(si.si_pid, &st, 0) < 0)
			return 1;
		if (!s)
			continue;

		s->exited = 1;
		s->last_status = st;
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
			say("%s exited with status 0", s->name);
		else if (WIFEXITED(st))
			warn("%s exited with status %d", s->name, WEXITSTATUS(st));
		else
			warn("%s killed by signal %d (%s)", s->name, WTERMSIG(st),
			     strsignal(WTERMSIG(st)));
		finished(s, WIFEXITED(st) && WEXITSTATUS(st) == 0, now);
	}
}

/* 1: all dependencies are ready, 0: wait, -1: a required one is down. */
static int deps_ready(const struct service *s)
{
	for (int d = 0; d < s->ndeps; d++) {
		const struct service *t = &svc[s->deps[d].idx];

		if (s->deps[d].hard) {
			if (t->state == ST_RUNNING)
				continue;
			if (t->state == ST_FAILED || (t->state == ST_INACTIVE && !t->want)) {
				warn("%s failed: dependency %s is not running", s->name, t->name);
				return -1;
			}
			return 0;
		}
		/* "after" only orders: wait while the other one is on its way. */
		if (t->state == ST_STARTING || (t->state == ST_INACTIVE && t->want))
			return 0;
	}
	return 1;
}

/* Ordering-only ("after") dependents count only at shutdown, where the
 * whole system stops in reverse order. */
static int has_active_dependents(int i, int soft)
{
	for (int j = 0; j < nsvc; j++) {
		if (svc[j].state != ST_STARTING && svc[j].state != ST_RUNNING &&
		    svc[j].state != ST_STOPPING)
			continue;
		for (int d = 0; d < svc[j].ndeps; d++)
			if (svc[j].deps[d].idx == i && (soft || svc[j].deps[d].hard))
				return 1;
	}
	return 0;
}

static void earliest(long long *next, long long t)
{
	if (*next < 0 || t < *next)
		*next = t;
}

/* Returns the time in ms until the next deadline, or -1 for none. */
long long services_step(long long now, int shutting_down)
{
	long long next = -1;
	int changed;

	do {
		changed = 0;
		for (int i = 0; i < nsvc; i++) {
			struct service *s = &svc[i];

			if (shutting_down)
				s->want = 0;
			switch (s->state) {
			case ST_INACTIVE:
				if (!s->want)
					break;
				if (s->deadline > now) {
					earliest(&next, s->deadline);
					break;
				}
				switch (deps_ready(s)) {
				case -1:
					s->state = ST_FAILED;
					s->want = 0;
					changed = 1;
					break;
				case 1:
					start_service(s, now);
					changed = 1;
					break;
				}
				break;
			case ST_STARTING:
			case ST_RUNNING:
				if (!s->want && !has_active_dependents(i, shutting_down)) {
					stop_service(s, now);
					changed = 1;
				} else if (s->state == ST_STARTING && s->ready_timeout_ms) {
					if (s->deadline <= now) {
						warn("%s not ready after %d ms", s->name, s->ready_timeout_ms);
						s->timed_out = 1;
						stop_service(s, now);
						changed = 1;
					} else {
						earliest(&next, s->deadline);
					}
				}
				break;
			case ST_STOPPING:
				if (s->deadline == LLONG_MAX)
					break;
				if (s->deadline <= now) {
					warn("%s did not stop in %d ms, sending SIGKILL", s->name,
					     s->stop_timeout_ms);
					kill(-s->pid, SIGKILL);
					s->deadline = LLONG_MAX;
				} else {
					earliest(&next, s->deadline);
				}
				break;
			}
		}
	} while (changed);

	for (int i = 0; i < nsvc && !shutting_down; i++)
		if (svc[i].critical && svc[i].state == ST_FAILED && emergency_shell()) {
			warn("critical service %s failed, starting emergency shell", svc[i].name);
			return 0; /* run a step at once to start the shell */
		}

	return next < 0 ? -1 : next - now;
}

int services_active(void)
{
	for (int i = 0; i < nsvc; i++)
		if (svc[i].pid)
			return 1;
	return 0;
}

int service_find(const char *name)
{
	for (int i = 0; i < nsvc; i++)
		if (strcmp(svc[i].name, name) == 0)
			return i;
	return -1;
}

/* Want i and, transitively, everything it requires. */
static const char *want_up(int i, char *visited)
{
	struct service *s = &svc[i];
	const char *err;

	if (s->error[0])
		return s->error;
	if (s->state == ST_STOPPING && !s->restarting)
		return "service is stopping; try again";
	if (visited[i])
		return NULL;
	visited[i] = 1;
	for (int d = 0; d < s->ndeps; d++)
		if (s->deps[d].hard && (err = want_up(s->deps[d].idx, visited)))
			return err;
	if (s->state == ST_FAILED)
		s->state = ST_INACTIVE;
	if (!s->want) {
		s->failures = 0;
		s->deadline = 0;
	}
	s->want = 1;
	return NULL;
}

/* Starting happens in services_step(), once dependencies are ready. */
const char *service_start(int i)
{
	char visited[MAX_SERVICES] = { 0 };

	return want_up(i, visited);
}

/* Unwant i and, transitively, everything that requires it. They stop in
 * services_step(), dependents first. */
void service_stop(int i)
{
	svc[i].want = 0;
	svc[i].restarting = 0;
	for (int j = 0; j < nsvc; j++)
		for (int d = 0; d < svc[j].ndeps; d++)
			if (svc[j].deps[d].idx == i && svc[j].deps[d].hard && svc[j].want)
				service_stop(j);
}

/* Dependents keep running while i restarts. */
const char *service_restart(int i, long long now)
{
	struct service *s = &svc[i];

	if (s->pid && (s->state == ST_STARTING || s->state == ST_RUNNING)) {
		s->restarting = 1;
		stop_service(s, now);
		return NULL;
	}
	if (!s->pid && s->state == ST_RUNNING) {
		s->state = ST_INACTIVE; /* finished oneshot: run it again */
		s->deadline = 0;
		return NULL;
	}
	return service_start(i);
}

/* Read from a readiness pipe. A newline means ready; then the pipe is
 * closed, as in the s6 protocol. */
static void read_notify(struct service *s)
{
	char buf[64];
	ssize_t r = -1;

	/* Bounded, so a service that floods the pipe cannot keep PID 1 here;
	 * poll() reports the rest next time. */
	for (int n = 0; n < 16 && (r = read(s->notify, buf, sizeof buf)) > 0; n++) {
		if (memchr(buf, '\n', (size_t)r)) {
			close(s->notify);
			s->notify = 0;
			if (s->state == ST_STARTING) {
				s->state = ST_RUNNING;
				say("%s is ready", s->name);
			}
			return;
		}
	}
	if (r == 0) {
		close(s->notify);
		s->notify = 0;
		if (s->state == ST_STARTING)
			warn("%s closed ready_fd without signaling readiness", s->name);
	}
}

int services_pollfds(struct pollfd *pfd)
{
	int n = 0;

	for (int i = 0; i < nsvc; i++)
		if (svc[i].notify)
			pfd[n++] = (struct pollfd){ .fd = svc[i].notify, .events = POLLIN };
	return n;
}

void services_poll_events(const struct pollfd *pfd, int n)
{
	for (int k = 0; k < n; k++)
		for (int i = 0; pfd[k].revents && i < nsvc; i++)
			if (svc[i].notify == pfd[k].fd)
				read_notify(&svc[i]);
}

/* argv points into cmdbuf, so a plain struct copy would point into the
 * source. */
static void copy_service(struct service *dst, const struct service *src)
{
	memcpy(dst, src, sizeof *dst);
	for (int k = 0; src->argv[k]; k++)
		dst->argv[k] = dst->cmdbuf + (src->argv[k] - src->cmdbuf);
}

static int find_in(const struct service *tab, int n, const char *name)
{
	for (int i = 0; i < n; i++)
		if (strcmp(tab[i].name, name) == 0)
			return i;
	return -1;
}

/* Re-read the service files. The reload is refused, and nothing changes,
 * if it would break a service that works now, or remove one that is still
 * active. New definitions of running services apply at their next start.
 * Returns 0 on success; msg gets the result either way. */
int services_reload(const char *dir, char *msg, size_t len)
{
	const size_t runtime = offsetof(struct service, state);
	/* A temporary mapping, so the idle RSS of PID 1 does not grow by a
	 * second service table after the first reload. */
	struct service *next = mmap(NULL, sizeof svc, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int n, added = 0, removed = 0, r = -1;

	if (next == MAP_FAILED) {
		snprintf(msg, len, "%s", strerror(errno));
		return -1;
	}
	n = load_table(dir, next);

	for (int i = 0; i < nsvc; i++) {
		int j = find_in(next, n, svc[i].name);

		if (svc[i].builtin)
			continue;
		if (j < 0) {
			if (svc[i].state != ST_INACTIVE && svc[i].state != ST_FAILED) {
				snprintf(msg, len, "%s was removed but is still active; stop it first",
					 svc[i].name);
				goto out;
			}
			removed++;
		} else if (next[j].error[0] && !svc[i].error[0]) {
			snprintf(msg, len, "service %s: %s", next[j].name, next[j].error);
			goto out;
		}
	}

	for (int j = 0; j < n; j++) {
		int i = find_in(svc, nsvc, next[j].name);

		if (i < 0 || svc[i].builtin) {
			added++;
			if (next[j].error[0]) {
				next[j].state = ST_FAILED;
				warn("service %s failed: %s", next[j].name, next[j].error);
			} else {
				next[j].want = 1;
			}
			continue;
		}
		memcpy((char *)&next[j] + runtime, (const char *)&svc[i] + runtime,
		       sizeof next[j] - runtime);
		if (next[j].type == TYPE_SIMPLE && next[j].state == ST_RUNNING && !next[j].pid) {
			/* A finished oneshot that is now simple: nothing runs, so
			 * it must not count as up. Start it. */
			next[j].state = ST_INACTIVE;
			next[j].deadline = 0;
		}
		if (svc[i].error[0] && !next[j].error[0]) {
			/* Fixed: try to start it now. */
			next[j].state = ST_INACTIVE;
			next[j].want = 1;
			next[j].failures = 0;
			next[j].deadline = 0;
		} else if (next[j].error[0]) {
			next[j].state = ST_FAILED;
		}
	}
	/* The emergency shell has no file; keep it while it runs or is
	 * waiting to restart. */
	for (int i = 0; i < nsvc; i++)
		if (svc[i].builtin && (svc[i].want || svc[i].pid))
			copy_service(&next[n++], &svc[i]);

	for (int j = 0; j < n; j++)
		copy_service(&svc[j], &next[j]);
	nsvc = n;
	snprintf(msg, len, "%d added, %d removed; running services use a new definition "
		 "from their next start", added, removed);
	say("reloaded: %s", msg);
	r = 0;
out:
	munmap(next, sizeof svc);
	return r;
}
