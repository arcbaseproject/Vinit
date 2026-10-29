/* Service file parser and dependency checks.
 *
 * Strict by design: an unknown section or key, a duplicate key, a
 * malformed value or an unsafe file permission rejects the whole file.
 * Rejected services stay in the table with an error so that dependents
 * report the real cause.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vinit.h"

enum { SEC_SERVICE, SEC_RESTART, SEC_DEPS, SEC_EXEC, SEC_LIMITS, NSECTIONS };
static const char *const sections[] = { "service", "restart", "dependencies", "execution",
					"limits" };

enum {
	K_NAME, K_COMMAND, K_TYPE, K_STOP_TIMEOUT, K_READY_FD, K_READY_TIMEOUT, K_CRITICAL,
	K_POLICY, K_DELAY, K_LIMIT,
	K_AFTER, K_REQUIRES,
	K_USER, K_GROUP, K_WORKDIR, K_ENV, K_STDIN, K_LOG,
	K_MEMORY_MAX, K_PIDS_MAX, K_CPU_MAX,
	NKEYS
};

static const struct {
	int section;
	const char *name;
	int repeat;
} keys[NKEYS] = {
	[K_NAME]         = { SEC_SERVICE, "name", 0 },
	[K_COMMAND]      = { SEC_SERVICE, "command", 0 },
	[K_TYPE]         = { SEC_SERVICE, "type", 0 },
	[K_STOP_TIMEOUT] = { SEC_SERVICE, "stop_timeout_ms", 0 },
	[K_READY_FD]     = { SEC_SERVICE, "ready_fd", 0 },
	[K_READY_TIMEOUT]= { SEC_SERVICE, "ready_timeout_ms", 0 },
	[K_CRITICAL]     = { SEC_SERVICE, "critical", 0 },
	[K_POLICY]       = { SEC_RESTART, "policy", 0 },
	[K_DELAY]        = { SEC_RESTART, "delay_ms", 0 },
	[K_LIMIT]        = { SEC_RESTART, "limit", 0 },
	[K_AFTER]        = { SEC_DEPS,    "after", 1 },
	[K_REQUIRES]     = { SEC_DEPS,    "requires", 1 },
	[K_USER]         = { SEC_EXEC,    "user", 0 },
	[K_GROUP]        = { SEC_EXEC,    "group", 0 },
	[K_WORKDIR]      = { SEC_EXEC,    "working_directory", 0 },
	[K_ENV]          = { SEC_EXEC,    "env", 1 },
	[K_STDIN]        = { SEC_EXEC,    "stdin", 0 },
	[K_LOG]          = { SEC_EXEC,    "log", 0 },
	[K_MEMORY_MAX]   = { SEC_LIMITS,  "memory_max", 0 },
	[K_PIDS_MAX]     = { SEC_LIMITS,  "pids_max", 0 },
	[K_CPU_MAX]      = { SEC_LIMITS,  "cpu_max", 0 },
};

static int fail(char *err, size_t len, const char *path, int line, const char *fmt, ...)
{
	char msg[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (line)
		snprintf(err, len, "%s:%d: %s", path, line, msg);
	else
		snprintf(err, len, "%s: %s", path, msg);
	return -1;
}

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

/* Service, user and group names: [A-Za-z0-9_][A-Za-z0-9_.@-]* */
static int valid_name(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n >= NAME_LEN || s[0] == '.' || s[0] == '-' || s[0] == '@')
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("_.@-", *s))
			return 0;
	return 1;
}

static const char *parse_int(const char *val, long min, long max, int *out)
{
	static char msg[64];
	char *end;
	long v;

	errno = 0;
	v = strtol(val, &end, 10);
	if (errno || *end || end == val || v < min || v > max) {
		snprintf(msg, sizeof msg, "must be an integer from %ld to %ld", min, max);
		return msg;
	}
	*out = (int)v;
	return NULL;
}

/* Bytes, with an optional K, M, G or T suffix (powers of 1024). */
static const char *parse_size(const char *val, long long *out)
{
	static const char suffixes[] = "KMGT";
	unsigned long long v;
	const char *p;
	char *end;
	int shift = 0;

	if (!isdigit((unsigned char)val[0]))
		return "must be a size such as 512M";
	errno = 0;
	v = strtoull(val, &end, 10);
	if (*end && !end[1] && (p = strchr(suffixes, *end)))
		shift = 10 * (int)(p - suffixes + 1);
	else if (*end)
		return "must be a size such as 512M";
	if (errno || v == 0 || v > (unsigned long long)LLONG_MAX >> shift)
		return "must be from 1 byte to 8E";
	*out = (long long)(v << shift);
	return NULL;
}

/* Split into argv. Words are separated by whitespace; double quotes group
 * words, and inside them \" and \\ are escapes. Characters that only mean
 * something to a shell are rejected, so nobody expects shell semantics. */
static const char *split_command(struct service *s, const char *p)
{
	char *out = s->cmdbuf, *end = s->cmdbuf + sizeof s->cmdbuf;
	int argc = 0;

	for (;;) {
		while (isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		if (argc == MAX_ARGS)
			return "too many arguments";
		s->argv[argc++] = out;
		while (*p && !isspace((unsigned char)*p)) {
			if (*p == '"') {
				for (p++; *p != '"'; p++) {
					if (!*p)
						return "unterminated quote";
					if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
						p++;
					if (out == end)
						return "command too long";
					*out++ = *p;
				}
				p++;
			} else if (strchr("'`$|;&<>\\", *p)) {
				return "shell syntax is not supported; use /bin/sh -c \"...\" explicitly";
			} else {
				if (out == end)
					return "command too long";
				*out++ = *p++;
			}
		}
		if (out == end)
			return "command too long";
		*out++ = '\0';
	}
	s->argv[argc] = NULL;
	if (argc == 0 || s->argv[0][0] != '/')
		return "must start with an absolute path";
	return NULL;
}

static const char *add_deps(struct service *s, char *val, int hard)
{
	for (char *tok = strtok(val, " \t"); tok; tok = strtok(NULL, " \t")) {
		struct dep *d = NULL;

		if (!valid_name(tok))
			return "invalid service name";
		if (strcmp(tok, s->name) == 0)
			return "a service cannot depend on itself";
		for (int i = 0; i < s->ndeps; i++)
			if (strcmp(s->deps[i].name, tok) == 0)
				d = &s->deps[i];
		if (!d) {
			if (s->ndeps == MAX_DEPS)
				return "too many dependencies";
			d = &s->deps[s->ndeps++];
			strcpy(d->name, tok);
			d->idx = -1;
		}
		d->hard |= hard;
	}
	return NULL;
}

static const char *add_env(struct service *s, const char *val)
{
	const char *eq = strchr(val, '=');

	if (s->nenv == MAX_ENV)
		return "too many entries";
	if (!eq || eq == val || isdigit((unsigned char)val[0]))
		return "must be NAME=value";
	for (const char *p = val; p < eq; p++)
		if (!isalnum((unsigned char)*p) && *p != '_')
			return "must be NAME=value";
	if (strlen(val) >= sizeof s->env[0])
		return "entry too long";
	strcpy(s->env[s->nenv++], val);
	return NULL;
}

static const char *copy_name(char *dst, const char *val)
{
	if (!valid_name(val))
		return "invalid name";
	strcpy(dst, val);
	return NULL;
}

static const char *apply(struct service *s, int k, char *val)
{
	const char *msg;
	size_t len;

	switch (k) {
	case K_NAME:
		return strcmp(val, s->name) ? "must match the file name" : NULL;
	case K_COMMAND:
		return split_command(s, val);
	case K_TYPE:
		if (strcmp(val, "simple") == 0)
			s->type = TYPE_SIMPLE;
		else if (strcmp(val, "oneshot") == 0)
			s->type = TYPE_ONESHOT;
		else
			return "must be simple or oneshot";
		return NULL;
	case K_STOP_TIMEOUT:
		return parse_int(val, 0, 3600000, &s->stop_timeout_ms);
	case K_READY_FD:
		return parse_int(val, 3, 1023, &s->ready_fd);
	case K_READY_TIMEOUT:
		return parse_int(val, 0, 3600000, &s->ready_timeout_ms);
	case K_CRITICAL:
		if (strcmp(val, "yes") == 0)
			s->critical = 1;
		else if (strcmp(val, "no") != 0)
			return "must be yes or no";
		return NULL;
	case K_POLICY:
		if (strcmp(val, "no") == 0)
			s->restart = RESTART_NO;
		else if (strcmp(val, "on-failure") == 0)
			s->restart = RESTART_ON_FAILURE;
		else if (strcmp(val, "always") == 0)
			s->restart = RESTART_ALWAYS;
		else
			return "must be no, on-failure or always";
		return NULL;
	case K_DELAY:
		return parse_int(val, 0, 3600000, &s->delay_ms);
	case K_LIMIT:
		return parse_int(val, 0, 1000000, &s->limit);
	case K_AFTER:
		return add_deps(s, val, 0);
	case K_REQUIRES:
		return add_deps(s, val, 1);
	case K_USER:
		return copy_name(s->user, val);
	case K_GROUP:
		return copy_name(s->group, val);
	case K_WORKDIR:
		if (val[0] != '/')
			return "must be an absolute path";
		if (strlen(val) >= sizeof s->workdir)
			return "path too long";
		strcpy(s->workdir, val);
		return NULL;
	case K_ENV:
		return add_env(s, val);
	case K_STDIN:
		if (strcmp(val, "null") == 0)
			s->stdin_console = 0;
		else if (strcmp(val, "console") == 0)
			s->stdin_console = 1;
		else
			return "must be null or console";
		return NULL;
	case K_MEMORY_MAX:
		return parse_size(val, &s->memory_max);
	case K_PIDS_MAX:
		return parse_int(val, 1, 4194304, &s->pids_max);
	case K_CPU_MAX:
		/* Percent of one CPU; 200% is two CPUs. */
		len = strlen(val);
		if (len < 2 || val[len - 1] != '%')
			return "must be a percentage such as 50%";
		val[len - 1] = '\0';
		return parse_int(val, 1, 100000, &s->cpu_max);
	case K_LOG:
		if (strpbrk(val, " \t"))
			return "must be one service name";
		if ((msg = add_deps(s, val, 0)))
			return msg;
		while (strcmp(s->deps[s->log_dep].name, val))
			s->log_dep++;
		s->log_dep++;
		return NULL;
	}
	return "unhandled key";
}

#define BAD(...) do { r = fail(err, errlen, path, lineno, __VA_ARGS__); goto out; } while (0)

int config_parse(const char *path, struct service *s, char *err, size_t errlen)
{
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	size_t blen = strlen(base);
	int section = -1, lineno = 0, r = 0, fd;
	unsigned seen = 0;
	char line[1024];
	struct stat st;
	FILE *f;

	memset(s, 0, sizeof *s);
	s->delay_ms = 1000;
	s->stop_timeout_ms = 5000;

	if (blen <= 5 || strcmp(base + blen - 5, ".conf") || blen - 5 >= NAME_LEN)
		return fail(err, errlen, path, 0, "file name must be <service>.conf");
	memcpy(s->name, base, blen - 5);
	if (!valid_name(s->name))
		return fail(err, errlen, path, 0, "invalid service name");

	/* O_NONBLOCK: opening a FIFO must not hang PID 1. */
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return fail(err, errlen, path, 0, "%s", strerror(errno));
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return fail(err, errlen, path, 0, "not a regular file");
	}
	if (st.st_uid != geteuid() || (st.st_mode & 022)) {
		close(fd);
		return fail(err, errlen, path, 0,
			    "must be owned by uid %d and not writable by group or others",
			    (int)geteuid());
	}
	if (!(f = fdopen(fd, "r"))) {
		close(fd);
		return fail(err, errlen, path, 0, "%s", strerror(errno));
	}

	while (fgets(line, sizeof line, f)) {
		size_t len = strlen(line);
		char *p, *eq, *key, *val;
		const char *msg;
		int k;

		lineno++;
		if (len && line[len - 1] == '\n')
			line[--len] = '\0';
		else if (!feof(f))
			BAD("line too long");
		p = trim(line);
		if (!*p || *p == '#' || *p == ';')
			continue;

		if (*p == '[') {
			len = strlen(p);
			if (p[len - 1] != ']')
				BAD("malformed section header");
			p[len - 1] = '\0';
			p = trim(p + 1);
			for (section = 0; section < NSECTIONS && strcmp(p, sections[section]); section++)
				;
			if (section == NSECTIONS)
				BAD("unknown section [%s]", p);
			continue;
		}

		if (!(eq = strchr(p, '=')))
			BAD("expected 'key = value'");
		*eq = '\0';
		key = trim(p);
		val = trim(eq + 1);
		if (section < 0)
			BAD("'%s' is outside of a section", key);
		for (k = 0; k < NKEYS && (keys[k].section != section || strcmp(keys[k].name, key)); k++)
			;
		if (k == NKEYS)
			BAD("unknown key '%s' in [%s]", key, sections[section]);
		if ((seen & 1u << k) && !keys[k].repeat)
			BAD("duplicate key '%s'", key);
		seen |= 1u << k;
		if (!*val)
			BAD("empty value for '%s'", key);
		if ((msg = apply(s, k, val)))
			BAD("%s: %s", key, msg);
	}
	if (ferror(f))
		BAD("read error");
	lineno = 0;
	if (!(seen & 1u << K_COMMAND))
		BAD("missing 'command' in [service]");
	if (s->ready_fd && s->type == TYPE_ONESHOT)
		BAD("ready_fd needs type = simple; a oneshot is ready when it exits");
	if ((seen & 1u << K_READY_TIMEOUT) && !s->ready_fd)
		BAD("ready_timeout_ms needs ready_fd");
out:
	fclose(f);
	return r;
}

static void mark_cycle(struct service *tab, const int *stack, int depth, int j)
{
	char msg[sizeof tab->error];
	int k = depth, len;

	while (k > 0 && stack[k] != j)
		k--;
	len = snprintf(msg, sizeof msg, "dependency cycle:");
	for (int m = k; m <= depth + 1 && len < (int)sizeof msg; m++)
		len += snprintf(msg + len, sizeof msg - (size_t)len, "%s %s", m > k ? " ->" : "",
				tab[m <= depth ? stack[m] : j].name);
	for (int m = k; m <= depth; m++)
		if (!tab[stack[m]].error[0])
			strcpy(tab[stack[m]].error, msg);
}

static void visit(struct service *tab, int i, char *color, int *stack, int depth)
{
	color[i] = 1; /* on the current path */
	stack[depth] = i;
	for (int d = 0; d < tab[i].ndeps; d++) {
		int j = tab[i].deps[d].idx;

		if (j < 0)
			continue;
		if (color[j] == 0)
			visit(tab, j, color, stack, depth + 1);
		else if (color[j] == 1)
			mark_cycle(tab, stack, depth, j);
	}
	color[i] = 2; /* done */
}

/* Resolve dependency names to indices and reject unknown names and
 * cycles. Both "after" and "requires" edges count for cycles. */
void config_resolve(struct service *tab, int n)
{
	char color[MAX_SERVICES];
	int stack[MAX_SERVICES];

	for (int i = 0; i < n; i++) {
		if (tab[i].error[0]) {
			tab[i].ndeps = 0;
			continue;
		}
		for (int d = 0; d < tab[i].ndeps; d++) {
			struct dep *dp = &tab[i].deps[d];

			for (int j = 0; j < n && dp->idx < 0; j++)
				if (strcmp(tab[j].name, dp->name) == 0)
					dp->idx = j;
			if (dp->idx < 0 && !tab[i].error[0])
				snprintf(tab[i].error, sizeof tab[i].error, "%s '%s' is not a known service",
					 tab[i].log_dep == d + 1 ? "log" : dp->hard ? "requires" : "after",
					 dp->name);
			else if (dp->idx >= 0 && tab[i].log_dep == d + 1 && tab[dp->idx].stdin_console &&
				 !tab[i].error[0])
				snprintf(tab[i].error, sizeof tab[i].error,
					 "log service '%s' reads stdin from the console", dp->name);
		}
	}
	/* Start a fresh search at every service: a completed branch can hide
	 * overlapping cycles, but every cyclic root has a path back to itself. */
	for (int i = 0; i < n; i++) {
		memset(color, 0, sizeof color);
		visit(tab, i, color, stack, 0);
	}
}
