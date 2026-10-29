/* Parse service files with vinit's own parser and resolver, and print
 * each error. Used to check files written by contrib tools. */
#define _GNU_SOURCE
#include <stdio.h>

#include "../src/vinit.h"

void say(const char *fmt, ...)
{
	(void)fmt;
}

static struct service tab[MAX_SERVICES];

int main(int argc, char **argv)
{
	int n = 0, bad = 0;

	if (argc < 2 || argc - 1 > MAX_SERVICES) {
		fprintf(stderr, "usage: conf-check FILE.conf... (at most %d)\n", MAX_SERVICES);
		return 2;
	}
	for (int i = 1; i < argc; i++, n++)
		config_parse(argv[i], &tab[n], tab[n].error, sizeof tab[n].error);
	config_resolve(tab, n);
	for (int i = 0; i < n; i++)
		if (tab[i].error[0]) {
			printf("%s: %s\n", tab[i].name[0] ? tab[i].name : argv[i + 1], tab[i].error);
			bad = 1;
		}
	return bad;
}
