#ifndef VINIT_H
#define VINIT_H

#include <stddef.h>
#include <sys/types.h>

#define SERVICE_DIR   "/etc/vinit/services"
#define MAX_SERVICES  64
#define MAX_ARGS      32
#define MAX_ENV       16
#define MAX_DEPS      16
#define NAME_LEN      32

enum svc_type { TYPE_SIMPLE, TYPE_ONESHOT };
enum restart  { RESTART_NO, RESTART_ON_FAILURE, RESTART_ALWAYS };
enum state    { ST_INACTIVE, ST_STARTING, ST_RUNNING, ST_STOPPING, ST_FAILED };

struct dep {
	char name[NAME_LEN];
	int idx;   /* index into the service table, set by config_resolve() */
	int hard;  /* requires (1) or after (0) */
};

struct service {
	/* Configuration. argv points into cmdbuf, so never copy a service. */
	char name[NAME_LEN];
	char cmdbuf[1024];
	char *argv[MAX_ARGS + 1];
	char env[MAX_ENV][256];
	int nenv;
	char user[NAME_LEN];
	char group[NAME_LEN];
	char workdir[256];
	int stdin_console;      /* stdin from the console instead of /dev/null */
	int critical;           /* failure starts the emergency shell */
	int type, restart, delay_ms, limit, stop_timeout_ms;
	int ready_fd;           /* fd number the service writes "\n" to when ready */
	int ready_timeout_ms;   /* 0: wait forever */
	struct dep deps[MAX_DEPS];
	int ndeps;
	char error[192]; /* set when the service is unusable; it never starts */

	/* Runtime state. Everything from here on survives a reload. */
	int state;
	int builtin;            /* the emergency shell; has no file */
	int want;               /* should be up */
	pid_t pid;              /* also the process group id */
	int failures;           /* consecutive, drives backoff and limit */
	int restarting;         /* stopping only to start again */
	int exited;             /* last_status is valid */
	int last_status;        /* wait status of the last exit */
	int notify;             /* our end of the readiness pipe, or 0 */
	int timed_out;          /* stopped for not becoming ready */
	long long started_at;
	long long deadline;     /* restart time, or stop timeout */
};

extern struct service svc[MAX_SERVICES];
extern int nsvc;

/* config.c */
int config_parse(const char *path, struct service *s, char *err, size_t errlen);
void config_resolve(struct service *svc, int n);

/* service.c */
struct pollfd;

void services_load(const char *dir);
int services_reload(const char *dir, char *msg, size_t len);
int services_pollfds(struct pollfd *pfd);
void services_poll_events(const struct pollfd *pfd, int n);
long long services_step(long long now, int shutting_down);
int services_active(void);
int reap_children(long long now);
int service_find(const char *name);
const char *service_start(int i);
void service_stop(int i);
const char *service_restart(int i, long long now);

/* control.c */
int control_open(void);
void control_handle(int fd);

/* vinit.c */
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* routine */
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* always shown */
void request_shutdown(int how);
extern int shutdown_how;

#endif
