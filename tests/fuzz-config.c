/* libFuzzer harness for src/config.c (needs clang):
 *
 *   make fuzz FUZZ_TIME=60
 *
 * The input is split at NUL bytes into up to 8 service files, a.conf to
 * h.conf. Each is parsed, then the set is resolved, so the fuzzer reaches
 * both the parser and the dependency and cycle checks. The harness also
 * checks the parser's output contract, so it finds wrong results, not only
 * crashes.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/vinit.h"

#define NFILES 8

void say(const char *fmt, ...)
{
	(void)fmt;
}

void warn(const char *fmt, ...)
{
	(void)fmt;
}

static char dir[] = "/tmp/vinit-fuzz.XXXXXX";
static struct service tab[NFILES];

static void check_contract(const struct service *s)
{
	int argc = 0;

	if (s->error[0])
		return;
	/* A parsed service always has an absolute command, NULL-terminated
	 * argv inside cmdbuf, and in-range values. */
	if (!s->argv[0] || s->argv[0][0] != '/')
		abort();
	for (; s->argv[argc]; argc++)
		if (s->argv[argc] < s->cmdbuf || s->argv[argc] >= s->cmdbuf + sizeof s->cmdbuf ||
		    !memchr(s->argv[argc], '\0', (size_t)(s->cmdbuf + sizeof s->cmdbuf - s->argv[argc])))
			abort();
	if (argc > MAX_ARGS || s->nenv > MAX_ENV || s->ndeps > MAX_DEPS)
		abort();
	if (s->ready_fd && (s->ready_fd < 3 || s->type != TYPE_SIMPLE))
		abort();
	if (strlen(s->name) >= NAME_LEN || s->delay_ms < 0 || s->stop_timeout_ms < 0)
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static int ready;
	const uint8_t *p = data, *end = data + size;
	char path[64];
	int n = 0;

	if (!ready) {
		if (!mkdtemp(dir))
			abort();
		ready = 1;
	}

	while (n < NFILES) {
		const uint8_t *z = memchr(p, '\0', (size_t)(end - p));
		size_t len = (size_t)((z ? z : end) - p);
		FILE *f;

		snprintf(path, sizeof path, "%s/%c.conf", dir, 'a' + n);
		if (!(f = fopen(path, "w")))
			abort();
		fwrite(p, 1, len, f);
		fclose(f);
		config_parse(path, &tab[n], tab[n].error, sizeof tab[n].error);
		check_contract(&tab[n]);
		n++;
		if (!z)
			break;
		p = z + 1;
	}

	config_resolve(tab, n);
	for (int i = 0; i < n; i++) {
		if (tab[i].error[0])
			continue;
		/* A service that resolves cleanly has valid dependency indices
		 * and never depends on itself. */
		for (int d = 0; d < tab[i].ndeps; d++)
			if (tab[i].deps[d].idx < 0 || tab[i].deps[d].idx >= n || tab[i].deps[d].idx == i)
				abort();
	}
	return 0;
}
