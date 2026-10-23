#ifndef MODULE_SCHED_H
#define MODULE_SCHED_H

#ifdef WEBIF

#include <stdint.h>
#include <time.h>
#include <stddef.h>

#define SCHED_MAX_JOBS   64
#define SCHED_MAX_STEPS  12
#define SCHED_MAX_TIMES  8
#define SCHED_NAME_LEN   64
#define SCHED_ACT_LEN    24
#define SCHED_ARG_LEN    400
#define SCHED_MSG_LEN    96

enum
{
	SCHED_EVERY = 0,  // every N seconds/minutes/hours/days
	SCHED_DAILY,      // every day at one or more times
	SCHED_WEEKLY,     // selected weekdays at one or more times
	SCHED_MONTHLY,    // a day of the month at one or more times
	SCHED_ONCE,       // a single date and time
	SCHED_STARTUP     // N seconds after OSCam starts
};

// What an action needs besides its name (used by the WebIF form)
enum
{
	SCHED_ARG_NONE = 0,
	SCHED_ARG_READER,    // a reader label, or * for all
	SCHED_ARG_USER,      // a user name
	SCHED_ARG_USERALL,   // a user name, or * for all
	SCHED_ARG_SCRIPT,    // script name + optional parameters
	SCHED_ARG_NUM,       // seconds
	SCHED_ARG_TEXT       // free text
};

typedef struct
{
	const char *act;
	const char *group;   // optgroup in the action list
	const char *label;
	int32_t     kind;    // SCHED_ARG_*
} sched_actdef_t;

typedef struct
{
	char act[SCHED_ACT_LEN];   // e.g. "reader.restart", "sleep", "log", "api"
	char arg[SCHED_ARG_LEN];   // target / parameter (may be empty)
} sched_step_t;

typedef struct
{
	uint32_t id;               // runtime id (changes on reload)
	char     name[SCHED_NAME_LEN];
	int8_t   enabled;
	int8_t   stype;            // SCHED_*
	int32_t  every_sec;        // SCHED_EVERY
	int8_t   ntimes;           // SCHED_DAILY/WEEKLY/MONTHLY
	int16_t  times[SCHED_MAX_TIMES]; // minutes since 00:00, sorted
	uint8_t  wdays;            // SCHED_WEEKLY: bit0=Sunday ... bit6=Saturday
	int8_t   mday;             // SCHED_MONTHLY: 1..31
	time_t   once;             // SCHED_ONCE
	int32_t  delay_sec;        // SCHED_STARTUP
	int8_t   nsteps;
	sched_step_t steps[SCHED_MAX_STEPS];

	// runtime state (not stored in task.cfg)
	time_t   next_run;         // 0 = never
	time_t   last_run;         // 0 = never
	uint32_t run_count;
	int8_t   last_ok;          // 1 = ok, 0 = failed (valid when last_run != 0)
	char     last_msg[SCHED_MSG_LEN];
	int8_t   running;
	int8_t   done;             // SCHED_ONCE / SCHED_STARTUP already fired
} sched_job_t;

void    sched_init(void);
void    sched_shutdown(void);

int8_t  sched_get_enabled(void);
void    sched_set_enabled(int8_t on);

int32_t sched_snapshot(sched_job_t *out, int32_t max);                 // copy of all jobs, returns count
uint32_t sched_job_save(const sched_job_t *job, uint32_t id, char *err, size_t errlen); // id=0 adds; returns id or 0
int8_t  sched_job_delete(uint32_t id);
int8_t  sched_job_toggle(uint32_t id);
int8_t  sched_job_run_now(uint32_t id);
int8_t  sched_reload(void);

const sched_actdef_t *sched_actdef(int32_t idx);                       // catalogue, NULL after the last entry
const sched_actdef_t *sched_actdef_find(const char *act);
char   *sched_raw_read(void);                                          // task.cfg as text (malloc'ed, never NULL)
int8_t  sched_raw_write(const char *text, char *err, size_t errlen);   // replace task.cfg and reload
int8_t  sched_is_known_action(const char *act);
const char *sched_action_label(const char *act);
void    sched_describe_schedule(const sched_job_t *j, char *out, size_t outlen);

// Implemented in module-sched-http.c
int32_t sched_api_call(const char *query, char *msg, size_t msglen);

// Implemented in module-webif.c (needs access to WebIf internals).
// Returns 0 on success, otherwise fills msg with the reason.
int32_t sched_exec_action(const char *act, const char *arg, char *msg, size_t msglen);

#endif // WEBIF

#endif // MODULE_SCHED_H
