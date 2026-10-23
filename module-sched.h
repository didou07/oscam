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
	SCHED_EVERY = 0,
	SCHED_DAILY,
	SCHED_WEEKLY,
	SCHED_MONTHLY,
	SCHED_ONCE,
	SCHED_STARTUP
};

enum
{
	SCHED_ARG_NONE = 0,
	SCHED_ARG_READER,
	SCHED_ARG_USER,
	SCHED_ARG_USERALL,
	SCHED_ARG_NUM,
	SCHED_ARG_TEXT
};

typedef struct
{
	const char *act;
	const char *label;
	int32_t     kind;
} sched_actdef_t;

typedef struct
{
	char act[SCHED_ACT_LEN];
	char arg[SCHED_ARG_LEN];
} sched_step_t;

typedef struct
{
	uint32_t id;
	char     name[SCHED_NAME_LEN];
	int8_t   enabled;
	int8_t   stype;
	int32_t  every_sec;
	int8_t   ntimes;
	int16_t  times[SCHED_MAX_TIMES];
	uint8_t  wdays;
	int8_t   mday;
	time_t   once;
	int32_t  delay_sec;
	int8_t   nsteps;
	sched_step_t steps[SCHED_MAX_STEPS];

	time_t   next_run;
	time_t   last_run;
	uint32_t run_count;
	int8_t   last_ok;
	char     last_msg[SCHED_MSG_LEN];
	int8_t   running;
	int8_t   done;
} sched_job_t;

void    sched_init(void);
void    sched_shutdown(void);

int8_t  sched_get_enabled(void);
void    sched_set_enabled(int8_t on);
void    sched_get_hidden(char *out, size_t n);
void    sched_set_hidden(const char *csv);

int32_t sched_snapshot(sched_job_t *out, int32_t max);
uint32_t sched_job_save(const sched_job_t *job, uint32_t id, char *err, size_t errlen);
int8_t  sched_job_delete(uint32_t id);
int8_t  sched_job_toggle(uint32_t id);
int8_t  sched_job_run_now(uint32_t id);
int8_t  sched_reload(void);

const sched_actdef_t *sched_actdef(int32_t idx);
const sched_actdef_t *sched_actdef_find(const char *act);
char   *sched_raw_read(void);
int8_t  sched_raw_write(const char *text, char *err, size_t errlen);
int8_t  sched_is_known_action(const char *act);
const char *sched_action_label(const char *act);
void    sched_describe_schedule(const sched_job_t *j, char *out, size_t outlen);

int32_t sched_api_call(const char *query, char *msg, size_t msglen);

int32_t sched_exec_action(const char *act, const char *arg, char *msg, size_t msglen);

#endif

#endif
