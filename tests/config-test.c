/* Unit tests for src/config.c. Runs as a normal user: config_parse()
 * accepts files owned by the effective uid, which is root for PID 1. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/vinit.h"

void say(const char *fmt, ...)
{
	(void)fmt;
}

static char dir[] = "/tmp/vinit-config-test.XXXXXX";
static int failures;

static void put(const char *name, const char *text, mode_t mode)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	if (!(f = fopen(path, "w"))) {
		perror(path);
		exit(1);
	}
	fputs(text, f);
	if (fchmod(fileno(f), mode) < 0) {
		perror("fchmod");
		exit(1);
	}
	fclose(f);
}

static int parse(struct service *s, const char *name, const char *text, mode_t mode)
{
	char path[256];

	put(name, text, mode);
	snprintf(path, sizeof path, "%s/%s", dir, name);
	return config_parse(path, s, s->error, sizeof s->error);
}

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* Expect rejection with an error message containing want. */
static void bad(const char *text, const char *want)
{
	static struct service s;

	if (parse(&s, "x.conf", text, 0644) == 0 || !strstr(s.error, want)) {
		printf("FAIL want error containing \"%s\", got \"%s\" for:\n%s\n", want, s.error, text);
		failures++;
	}
}

static struct service tab[8];

int main(void)
{
	struct service *s = &tab[0];

	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}

	/* The example from the specification. */
	CHECK(parse(s, "sshd.conf",
		    "[service]\nname = sshd\ncommand = /usr/sbin/sshd -D\ntype = simple\n\n"
		    "[restart]\npolicy = on-failure\ndelay_ms = 1000\n\n"
		    "[dependencies]\nafter = network\nrequires = network\n\n"
		    "[execution]\nuser = root\nworking_directory = /\n", 0644) == 0);
	CHECK(strcmp(s->name, "sshd") == 0);
	CHECK(strcmp(s->argv[0], "/usr/sbin/sshd") == 0);
	CHECK(strcmp(s->argv[1], "-D") == 0 && s->argv[2] == NULL);
	CHECK(s->type == TYPE_SIMPLE && s->restart == RESTART_ON_FAILURE && s->delay_ms == 1000);
	CHECK(s->ndeps == 1 && strcmp(s->deps[0].name, "network") == 0 && s->deps[0].hard);
	CHECK(strcmp(s->user, "root") == 0 && strcmp(s->workdir, "/") == 0);

	/* Quoting, comments, whitespace, repeated keys. */
	CHECK(parse(s, "q.conf",
		    "# comment\n; comment\n  [ service ]  \n"
		    "command = /bin/sh -c \"echo \\\"hi\\\" there\" a\"b c\"\n"
		    "type = oneshot\n"
		    "[execution]\nenv = A=1\nenv = B=x y\nstdin = console\n"
		    "[dependencies]\nafter = a b\nafter = c\nrequires = b\n", 0644) == 0);
	CHECK(strcmp(s->argv[2], "echo \"hi\" there") == 0);
	CHECK(strcmp(s->argv[3], "ab c") == 0 && s->argv[4] == NULL);
	CHECK(s->type == TYPE_ONESHOT && s->stdin_console);
	CHECK(s->nenv == 2 && strcmp(s->env[1], "B=x y") == 0);
	CHECK(s->ndeps == 3 && !s->deps[0].hard && s->deps[1].hard && !s->deps[2].hard);

	bad("[service]\ncolour = blue\ncommand = /bin/true\n", "x.conf:2: unknown key 'colour' in [service]");
	bad("[servcie]\n", "x.conf:1: unknown section [servcie]");
	bad("command = /bin/true\n", "outside of a section");
	bad("[service]\ncommand = /bin/true\ncommand = /bin/false\n", "duplicate key 'command'");
	bad("[service]\ntype = simple\n", "missing 'command'");
	bad("[service]\ncommand = true\n", "absolute path");
	bad("[service]\ncommand = /bin/echo hi | wc\n", "shell syntax is not supported");
	bad("[service]\ncommand = /bin/echo $HOME\n", "shell syntax is not supported");
	bad("[service]\ncommand = /bin/echo \"hi\n", "unterminated quote");
	bad("[service]\ncommand =\n", "empty value");
	bad("[service]\ncommand /bin/true\n", "expected 'key = value'");
	bad("[service\n", "malformed section header");
	bad("[service]\nname = y\ncommand = /bin/true\n", "must match the file name");
	bad("[service]\ncommand = /bin/true\ntype = forking\n", "must be simple or oneshot");
	bad("[service]\ncommand = /bin/true\n[restart]\ndelay_ms = 10x\n", "must be an integer");
	bad("[service]\ncommand = /bin/true\n[restart]\ndelay_ms = -1\n", "must be an integer");
	bad("[service]\ncommand = /bin/true\n[restart]\npolicy = sometimes\n", "must be no, on-failure or always");
	bad("[service]\ncommand = /bin/true\n[dependencies]\nrequires = x\n", "cannot depend on itself");
	bad("[service]\ncommand = /bin/true\n[dependencies]\nrequires = ../etc\n", "invalid service name");
	bad("[service]\ncommand = /bin/true\n[execution]\nenv = 1A=b\n", "must be NAME=value");
	bad("[service]\ncommand = /bin/true\n[execution]\nenv = novalue\n", "must be NAME=value");
	bad("[service]\ncommand = /bin/true\n[execution]\nworking_directory = tmp\n", "absolute path");
	bad("[service]\ncommand = /bin/true\n[execution]\nuser = r;oot\n", "invalid name");
	bad("[service]\ncommand = /bin/true\nready_fd = 2\n", "must be an integer from 3 to 1023");
	bad("[service]\nready_fd = 3\ncommand = /bin/true\ntype = oneshot\n", "ready_fd needs type = simple");
	bad("[service]\ncommand = /bin/true\nready_timeout_ms = 10\n", "ready_timeout_ms needs ready_fd");
	CHECK(parse(s, "r.conf", "[service]\ncommand = /bin/true\nready_fd = 3\nready_timeout_ms = 500\n", 0644) == 0);
	CHECK(s->ready_fd == 3 && s->ready_timeout_ms == 500);
	bad("[service]\ncommand = /bin/true\ncritical = maybe\n", "critical: must be yes or no");
	CHECK(parse(s, "c.conf", "[service]\ncommand = /bin/true\ncritical = yes\n", 0644) == 0 && s->critical);

	{
		char longline[1100];

		memset(longline, 'a', sizeof longline);
		memcpy(longline, "[service]\n#", 11);
		strcpy(longline + sizeof longline - 2, "\n");
		bad(longline, "line too long");
	}

	CHECK(parse(s, "w.conf", "[service]\ncommand = /bin/true\n", 0666) < 0);
	CHECK(strstr(s->error, "not writable by group or others") != NULL);
	CHECK(parse(s, "bad name.conf", "[service]\ncommand = /bin/true\n", 0644) < 0);
	CHECK(parse(s, "noext", "[service]\ncommand = /bin/true\n", 0644) < 0);

	/* Dependency resolution: a <-> b cycle, c needs an unknown service,
	 * d needs a, e is fine. */
	CHECK(parse(&tab[0], "a.conf", "[service]\ncommand = /bin/true\n[dependencies]\nrequires = b\n", 0644) == 0);
	CHECK(parse(&tab[1], "b.conf", "[service]\ncommand = /bin/true\n[dependencies]\nafter = a\n", 0644) == 0);
	CHECK(parse(&tab[2], "c.conf", "[service]\ncommand = /bin/true\n[dependencies]\nrequires = zz\n", 0644) == 0);
	CHECK(parse(&tab[3], "d.conf", "[service]\ncommand = /bin/true\n[dependencies]\nrequires = a\n", 0644) == 0);
	CHECK(parse(&tab[4], "e.conf", "[service]\ncommand = /bin/true\n[dependencies]\nrequires = d\n", 0644) == 0);
	config_resolve(tab, 5);
	CHECK(strcmp(tab[0].error, "dependency cycle: a -> b -> a") == 0);
	CHECK(strcmp(tab[1].error, "dependency cycle: a -> b -> a") == 0);
	CHECK(strcmp(tab[2].error, "requires 'zz' is not a known service") == 0);
	CHECK(tab[3].error[0] == '\0' && tab[3].deps[0].idx == 0);
	CHECK(tab[4].error[0] == '\0' && tab[4].deps[0].idx == 3);

	/* The second branch overlaps the first cycle through a completed node.
	 * A service depending on the cycle is not itself cyclic. */
	CHECK(parse(&tab[0], "a.conf", "[service]\ncommand = /bin/true\n[dependencies]\nafter = b c\n", 0644) == 0);
	CHECK(parse(&tab[1], "b.conf", "[service]\ncommand = /bin/true\n[dependencies]\nafter = a\n", 0644) == 0);
	CHECK(parse(&tab[2], "c.conf", "[service]\ncommand = /bin/true\n[dependencies]\nafter = b\n", 0644) == 0);
	CHECK(parse(&tab[3], "d.conf", "[service]\ncommand = /bin/true\n[dependencies]\nrequires = c\n", 0644) == 0);
	config_resolve(tab, 4);
	CHECK(strcmp(tab[0].error, "dependency cycle: a -> b -> a") == 0);
	CHECK(strcmp(tab[1].error, "dependency cycle: a -> b -> a") == 0);
	CHECK(strcmp(tab[2].error, "dependency cycle: b -> a -> c -> b") == 0);
	CHECK(tab[3].error[0] == '\0' && tab[3].deps[0].idx == 2);

	/* log = X is an ordering dependency on X, which gets the output. */
	bad("[service]\ncommand = /bin/true\n[execution]\nlog = a b\n", "must be one service name");
	bad("[service]\ncommand = /bin/true\n[execution]\nlog = x\n", "cannot depend on itself");

	/* Resource limits. */
	CHECK(parse(s, "lim.conf", "[service]\ncommand = /bin/true\n[limits]\n"
		    "memory_max = 512M\npids_max = 64\ncpu_max = 250%\n", 0644) == 0);
	CHECK(s->memory_max == 512LL << 20 && s->pids_max == 64 && s->cpu_max == 250);
	CHECK(parse(s, "lim.conf", "[service]\ncommand = /bin/true\n[limits]\n"
		    "memory_max = 4096\n", 0644) == 0);
	CHECK(s->memory_max == 4096 && s->pids_max == 0 && s->cpu_max == 0);
	CHECK(parse(s, "lim.conf", "[service]\ncommand = /bin/true\n[limits]\n"
		    "memory_max = 7T\n", 0644) == 0);
	CHECK(s->memory_max == 7LL << 40);
	bad("[service]\ncommand = /bin/true\n[limits]\nmemory_max = 0\n", "from 1 byte");
	bad("[service]\ncommand = /bin/true\n[limits]\nmemory_max = 1X\n", "size such as");
	bad("[service]\ncommand = /bin/true\n[limits]\nmemory_max = 1MB\n", "size such as");
	bad("[service]\ncommand = /bin/true\n[limits]\nmemory_max = -1\n", "size such as");
	bad("[service]\ncommand = /bin/true\n[limits]\nmemory_max = 9999999999T\n", "from 1 byte");
	bad("[service]\ncommand = /bin/true\n[limits]\npids_max = 0\n", "from 1 to");
	bad("[service]\ncommand = /bin/true\n[limits]\ncpu_max = 50\n", "percentage");
	bad("[service]\ncommand = /bin/true\n[limits]\ncpu_max = %\n", "percentage");
	bad("[service]\ncommand = /bin/true\n[limits]\ncpu_max = 0%\n", "from 1 to");
	bad("[service]\ncommand = /bin/true\n[limits]\nnice = 5\n", "unknown key 'nice' in [limits]");
	CHECK(parse(&tab[0], "a.conf", "[service]\ncommand = /bin/true\n[dependencies]\nafter = c\n"
		    "[execution]\nlog = b\n", 0644) == 0);
	CHECK(tab[0].log_dep == 2 && !tab[0].deps[1].hard);
	CHECK(parse(&tab[1], "b.conf", "[service]\ncommand = /bin/true\n", 0644) == 0);
	CHECK(parse(&tab[2], "c.conf", "[service]\ncommand = /bin/true\n[execution]\nlog = d\n", 0644) == 0);
	CHECK(parse(&tab[3], "d.conf", "[service]\ncommand = /bin/true\n[execution]\nstdin = console\n", 0644) == 0);
	CHECK(parse(&tab[4], "e.conf", "[service]\ncommand = /bin/true\n[execution]\nlog = zz\n", 0644) == 0);
	config_resolve(tab, 5);
	CHECK(tab[0].error[0] == '\0' && tab[0].deps[1].idx == 1);
	CHECK(strcmp(tab[2].error, "log service 'd' reads stdin from the console") == 0);
	CHECK(strcmp(tab[4].error, "log 'zz' is not a known service") == 0);

	{
		char cmd[64];

		snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
		(void)!system(cmd);
	}
	if (failures)
		return 1;
	printf("config test: PASS\n");
	return 0;
}
