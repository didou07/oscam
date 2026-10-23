#define MODULE_LOG_PREFIX "sched"

#include "globals.h"

#ifdef WEBIF

#include "module-sched.h"
#include "oscam-string.h"
#include "oscam-time.h"

// Task scheduler: jobs are read from <confdir>/task.cfg and run in worker threads.

extern char cs_confdir[];

static pthread_mutex_t sched_mx = PTHREAD_MUTEX_INITIALIZER;
static sched_job_t *jobs = NULL;
static int32_t job_count = 0;
static uint32_t next_id = 1;
static int8_t master_enabled = 1;
static time_t start_time = 0;

static volatile int8_t sched_running = 0;
static int8_t sched_thread_started = 0;
static pthread_t sched_thread;
static volatile int32_t workers = 0;

static const sched_actdef_t actions[] =
{
	{ "reader.restart",         "Reader",     "Restart reader",             SCHED_ARG_READER },
	{ "reader.enable",          "Reader",     "Enable reader",              SCHED_ARG_READER },
	{ "reader.disable",         "Reader",     "Disable reader",             SCHED_ARG_READER },
	{ "user.enable",            "User",       "Enable user",                SCHED_ARG_USER },
	{ "user.disable",           "User",       "Disable user",               SCHED_ARG_USER },
	{ "user.kick",              "User",       "Disconnect user",            SCHED_ARG_USERALL },
	{ "user.resetstats",        "User",       "Reset user statistics",      SCHED_ARG_USERALL },
	{ "system.reload_readers",  "OSCam",      "Reload readers",             SCHED_ARG_NONE },
	{ "system.reload_users",    "OSCam",      "Reload users",               SCHED_ARG_NONE },
	{ "system.reload_services", "OSCam",      "Reload services",            SCHED_ARG_NONE },
	{ "system.restart",         "OSCam",      "Restart OSCam",              SCHED_ARG_NONE },
	{ "system.shutdown",        "OSCam",      "Shut down OSCam",            SCHED_ARG_NONE },
	{ "stats.reset_readers",    "Statistics", "Reset reader statistics",    SCHED_ARG_NONE },
	{ "stats.reset_users",      "Statistics", "Reset all user statistics",  SCHED_ARG_NONE },
	{ "stats.reset_clients",    "Statistics", "Reset client statistics",    SCHED_ARG_NONE },
	{ "stats.reset_cacheex",    "Statistics", "Reset CacheEX statistics",   SCHED_ARG_NONE },
	{ "cache.clean",            "Other",      "Clean ECM cache",            SCHED_ARG_NONE },
	{ "failban.clear",          "Other",      "Clear failban list",         SCHED_ARG_NONE },
	{ "script.run",             "Other",      "Run script",                 SCHED_ARG_SCRIPT },
	{ "log",                    "Other",      "Write log message",          SCHED_ARG_TEXT },
	{ "sleep",                  "Other",      "Wait (seconds)",             SCHED_ARG_NUM },
	{ "api",                    "Other",      "WebIF request",              SCHED_ARG_TEXT },
	{ NULL, NULL, NULL, 0 }
};

static const char *wday_names[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
static const char *wday_long[]  = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

const sched_actdef_t *sched_actdef(int32_t idx)
{
	int32_t i;
	for(i = 0; actions[i].act; i++)
		{ if(i == idx) { return &actions[i]; } }
	return NULL;
}

const sched_actdef_t *sched_actdef_find(const char *act)
{
	int32_t i;
	for(i = 0; actions[i].act; i++)
		{ if(!strcmp(actions[i].act, act)) { return &actions[i]; } }
	return NULL;
}

int8_t sched_is_known_action(const char *act)
{
	return sched_actdef_find(act) != NULL;
}

const char *sched_action_label(const char *act)
{
	const sched_actdef_t *d = sched_actdef_find(act);
	return d ? d->label : act;
}

/* ---------------------------------------------------------------- time */

static int32_t days_in_month(int32_t year, int32_t mon0)
{
	static const int8_t dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if(mon0 == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) { return 29; }
	return dm[mon0];
}

static int cmp_i16(const void *a, const void *b)
{
	return (int)*(const int16_t *)a - (int)*(const int16_t *)b;
}

// Next run strictly after "from" for calendar based schedules; 0 if none.
static time_t next_calendar(const sched_job_t *j, time_t from)
{
	struct tm base;
	int32_t off, t;
	localtime_r(&from, &base);

	for(off = 0; off < 400; off++)
	{
		struct tm d = base;
		int32_t ok = 0;
		time_t best = 0;

		d.tm_mday += off;
		d.tm_hour = 12; d.tm_min = 0; d.tm_sec = 0; d.tm_isdst = -1;
		if(mktime(&d) == (time_t)-1) { continue; } // also normalizes d

		switch(j->stype)
		{
			case SCHED_DAILY:
				ok = 1;
				break;
			case SCHED_WEEKLY:
				ok = (j->wdays >> d.tm_wday) & 1;
				break;
			case SCHED_MONTHLY:
			{
				int32_t dim = days_in_month(d.tm_year + 1900, d.tm_mon);
				ok = (d.tm_mday == j->mday) || (j->mday > dim && d.tm_mday == dim);
				break;
			}
			default:
				return 0;
		}
		if(!ok) { continue; }

		for(t = 0; t < j->ntimes; t++)
		{
			struct tm c = d;
			time_t ct;
			c.tm_hour = j->times[t] / 60; c.tm_min = j->times[t] % 60; c.tm_sec = 0; c.tm_isdst = -1;
			ct = mktime(&c);
			if(ct != (time_t)-1 && ct > from && (!best || ct < best)) { best = ct; }
		}
		if(best) { return best; }
	}
	return 0;
}

// Next run after "from" (job already fired at "from" or is being set up).
static time_t compute_next(const sched_job_t *j, time_t from)
{
	switch(j->stype)
	{
		case SCHED_EVERY:   return j->every_sec > 0 ? from + j->every_sec : 0;
		case SCHED_DAILY:
		case SCHED_WEEKLY:
		case SCHED_MONTHLY: return next_calendar(j, from);
		case SCHED_ONCE:    return j->once > from ? j->once : 0;
		case SCHED_STARTUP: return 0;
	}
	return 0;
}

void sched_describe_schedule(const sched_job_t *j, char *out, size_t outlen)
{
	char tl[96] = "", dl[64] = "";
	int32_t i;
	size_t n = 0;

	for(i = 0; i < j->ntimes; i++)
		{ n += (size_t)snprintf(tl + n, sizeof(tl) - n, "%s%02d:%02d", i ? ", " : "", j->times[i] / 60, j->times[i] % 60); if(n >= sizeof(tl)) { break; } }

	switch(j->stype)
	{
		case SCHED_EVERY:
		{
			int32_t s = j->every_sec;
			if(s % 86400 == 0)      { snprintf(out, outlen, "Every %d day%s", s / 86400, s / 86400 == 1 ? "" : "s"); }
			else if(s % 3600 == 0)  { snprintf(out, outlen, "Every %d hour%s", s / 3600, s / 3600 == 1 ? "" : "s"); }
			else if(s % 60 == 0)    { snprintf(out, outlen, "Every %d min", s / 60); }
			else                    { snprintf(out, outlen, "Every %d s", s); }
			break;
		}
		case SCHED_DAILY:
			snprintf(out, outlen, "Daily at %s", tl);
			break;
		case SCHED_WEEKLY:
			n = 0;
			for(i = 1; i <= 7; i++)
				{ if((j->wdays >> (i % 7)) & 1) { n += (size_t)snprintf(dl + n, sizeof(dl) - n, "%s%s", n ? ", " : "", wday_long[i % 7]); } }
			snprintf(out, outlen, "%s at %s", dl, tl);
			break;
		case SCHED_MONTHLY:
			snprintf(out, outlen, "Monthly on day %d at %s", j->mday, tl);
			break;
		case SCHED_ONCE:
		{
			struct tm tm;
			localtime_r(&j->once, &tm);
			snprintf(out, outlen, "Once on %04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
			break;
		}
		case SCHED_STARTUP:
			snprintf(out, outlen, "%d s after start", j->delay_sec);
			break;
		default:
			snprintf(out, outlen, "-");
	}
}

/* -------------------------------------------------------------- parsing */

static int8_t parse_times(const char *val, sched_job_t *j)
{
	char buf[128], *p, *save = NULL;
	cs_strncpy(buf, val, sizeof(buf));
	j->ntimes = 0;
	for(p = strtok_r(buf, ", ", &save); p && j->ntimes < SCHED_MAX_TIMES; p = strtok_r(NULL, ", ", &save))
	{
		int32_t h, m;
		if(sscanf(p, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60)
			{ j->times[(int)j->ntimes++] = (int16_t)(h * 60 + m); }
	}
	if(j->ntimes > 1) { qsort(j->times, (size_t)j->ntimes, sizeof(int16_t), cmp_i16); }
	return j->ntimes > 0;
}

static int8_t parse_days(const char *val, uint8_t *mask)
{
	char buf[64], *p, *save = NULL;
	int32_t i;
	cs_strncpy(buf, val, sizeof(buf));
	*mask = 0;
	for(p = strtok_r(buf, ", ", &save); p; p = strtok_r(NULL, ", ", &save))
		{ for(i = 0; i < 7; i++) { if(!strncasecmp(p, wday_names[i], 3)) { *mask |= (uint8_t)(1 << i); } } }
	return *mask != 0;
}

static int32_t parse_every(const char *val)
{
	char *end = NULL;
	long n = strtol(val, &end, 10);
	if(n <= 0) { return 0; }
	if(end && *end)
	{
		switch(tolower((unsigned char)*end))
		{
			case 'm': n *= 60; break;
			case 'h': n *= 3600; break;
			case 'd': n *= 86400; break;
			default: break; // 's' or nothing
		}
	}
	return n > 31536000 ? 31536000 : (int32_t)n;
}

static time_t parse_datetime(const char *val)
{
	struct tm tm;
	int32_t y, mo, d, h = 0, mi = 0, s = 0;
	if(sscanf(val, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 5) { return 0; }
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
	tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s; tm.tm_isdst = -1;
	return mktime(&tm);
}

static void add_step(sched_job_t *j, const char *act, const char *arg)
{
	if(j->nsteps >= SCHED_MAX_STEPS) { cs_log("[%s] too many steps (max %d), '%s' ignored", j->name, SCHED_MAX_STEPS, act); return; }
	cs_strncpy(j->steps[(int)j->nsteps].act, act, SCHED_ACT_LEN);
	cs_strncpy(j->steps[(int)j->nsteps].arg, arg ? arg : "", SCHED_ARG_LEN);
	j->nsteps++;
}

static void add_do_line(sched_job_t *j, const char *val)
{
	char act[SCHED_ACT_LEN];
	const char *arg = val;
	size_t n = 0;
	while(*arg && !isspace((unsigned char)*arg) && n < sizeof(act) - 1) { act[n++] = *arg++; }
	act[n] = '\0';
	while(*arg && isspace((unsigned char)*arg)) { arg++; }
	if(!sched_is_known_action(act)) { cs_log("[%s] unknown action '%s' ignored", j->name, act); return; }
	add_step(j, act, arg);
}

// Fill defaults / validate. Returns 0 and fills err if the job is unusable.
static int8_t finalize_job(sched_job_t *j, int8_t has_schedule_key, int8_t has_every, int8_t has_time, int8_t has_days, int8_t has_date, int8_t has_delay, char *err, size_t errlen)
{
	if(!has_schedule_key)
	{
		if(has_date) { j->stype = SCHED_ONCE; }
		else if(has_time && has_days) { j->stype = SCHED_WEEKLY; }
		else if(has_time) { j->stype = SCHED_DAILY; }
		else if(has_every) { j->stype = SCHED_EVERY; }
		else if(has_delay) { j->stype = SCHED_STARTUP; }
		else { snprintf(err, errlen, "no schedule"); return 0; }
	}
	if(j->nsteps == 0) { snprintf(err, errlen, "no actions"); return 0; }
	switch(j->stype)
	{
		case SCHED_EVERY:   if(j->every_sec < 1) { snprintf(err, errlen, "invalid interval"); return 0; } break;
		case SCHED_DAILY:   if(j->ntimes < 1) { snprintf(err, errlen, "no time"); return 0; } break;
		case SCHED_WEEKLY:  if(j->ntimes < 1 || !j->wdays) { snprintf(err, errlen, "no day or time"); return 0; } break;
		case SCHED_MONTHLY: if(j->ntimes < 1 || j->mday < 1 || j->mday > 31) { snprintf(err, errlen, "no day or time"); return 0; } break;
		case SCHED_ONCE:    if(!j->once) { snprintf(err, errlen, "invalid date"); return 0; } break;
		case SCHED_STARTUP: if(j->delay_sec < 0) { j->delay_sec = 0; } break;
		default: snprintf(err, errlen, "invalid schedule"); return 0;
	}
	return 1;
}

// Parse task.cfg into tmp[] (max SCHED_MAX_JOBS). Returns number of jobs, sets *enabled.
static int32_t parse_file(sched_job_t *tmp, int8_t *enabled)
{
	char path[300], line[1200];
	FILE *fp;
	int32_t n = 0;
	sched_job_t *cur = NULL;
	int8_t in_sched = 0, h_sched = 0, h_every = 0, h_time = 0, h_days = 0, h_date = 0, h_delay = 0;
	char err[64];

	snprintf(path, sizeof(path), "%stask.cfg", cs_confdir);
	fp = fopen(path, "r");
	if(!fp) { return 0; }

#define CLOSE_JOB() do { \
		if(cur) { \
			if(!finalize_job(cur, h_sched, h_every, h_time, h_days, h_date, h_delay, err, sizeof(err))) \
				{ cs_log("job '%s' ignored: %s", cur->name, err); n--; } \
			cur = NULL; \
		} } while(0)

	while(fgets(line, sizeof(line), fp))
	{
		char *key, *val, *eq, name[SCHED_NAME_LEN];
		trim(line);
		if(!*line || *line == '#' || *line == ';') { continue; }

		if(!strcasecmp(line, "[scheduler]")) { CLOSE_JOB(); in_sched = 1; continue; }
		if(sscanf(line, "[job:%63[^]]]", name) == 1)
		{
			CLOSE_JOB();
			in_sched = 0;
			if(n >= SCHED_MAX_JOBS) { cs_log("job limit (%d) reached, '%s' ignored", SCHED_MAX_JOBS, name); break; }
			cur = &tmp[n++];
			memset(cur, 0, sizeof(*cur));
			trim(name);
			cs_strncpy(cur->name, name, SCHED_NAME_LEN);
			cur->enabled = 1;
			cur->stype = SCHED_DAILY;
			h_sched = h_every = h_time = h_days = h_date = h_delay = 0;
			continue;
		}

		eq = strchr(line, '=');
		if(!eq) { continue; }
		*eq = '\0';
		key = line; val = eq + 1;
		trim(key); trim(val);
		strtolower(key);
		if(!*val) { continue; }

		if(in_sched)
		{
			if(!strcmp(key, "enabled")) { *enabled = atoi(val) ? 1 : 0; }
			continue;
		}
		if(!cur) { continue; }

		if(!strcmp(key, "enabled")) { cur->enabled = atoi(val) ? 1 : 0; }
		else if(!strcmp(key, "schedule"))
		{
			strtolower(val);
			h_sched = 1;
			if(!strcmp(val, "every")) { cur->stype = SCHED_EVERY; }
			else if(!strcmp(val, "daily")) { cur->stype = SCHED_DAILY; }
			else if(!strcmp(val, "weekly")) { cur->stype = SCHED_WEEKLY; }
			else if(!strcmp(val, "monthly")) { cur->stype = SCHED_MONTHLY; }
			else if(!strcmp(val, "once")) { cur->stype = SCHED_ONCE; }
			else if(!strcmp(val, "startup")) { cur->stype = SCHED_STARTUP; }
			else { h_sched = 0; }
		}
		else if(!strcmp(key, "every")) { cur->every_sec = parse_every(val); h_every = 1; }
		else if(!strcmp(key, "interval")) { cur->every_sec = atoi(val); h_every = 1; } // NCam
		else if(!strcmp(key, "time")) { if(parse_times(val, cur)) { h_time = 1; } }
		else if(!strcmp(key, "days")) { if(parse_days(val, &cur->wdays)) { h_days = 1; } }
		else if(!strcmp(key, "weekly")) // NCam: "<day> HH:MM"
		{
			char d[16], t[16];
			if(sscanf(val, "%15s %15s", d, t) == 2 && parse_days(d, &cur->wdays) && parse_times(t, cur)) { h_days = 1; h_time = 1; }
		}
		else if(!strcmp(key, "day")) { cur->mday = (int8_t)atoi(val); }
		else if(!strcmp(key, "date") || !strcmp(key, "datetime"))
			{ cur->once = parse_datetime(val); h_date = 1; }
		else if(!strcmp(key, "delay") || !strcmp(key, "after_boot")) { cur->delay_sec = atoi(val); h_delay = 1; }
		else if(!strcmp(key, "do")) { add_do_line(cur, val); }
		else if(!strcmp(key, "api")) { add_step(cur, "api", val); }   // NCam
		else if(!strcmp(key, "restart")) { if(atoi(val) == 1) { add_step(cur, "system.restart", ""); } } // NCam
		else if(!strcmp(key, "log")) { add_step(cur, "log", val); }   // NCam
		else if(!strcmp(key, "sleep")) { if(atoi(val) > 0) { add_step(cur, "sleep", val); } } // NCam
	}
	CLOSE_JOB();
#undef CLOSE_JOB
	fclose(fp);
	return n;
}

/* -------------------------------------------------------------- writing */

static void clean_text(char *s)
{
	for(; *s; s++) { if(*s == '\n' || *s == '\r') { *s = ' '; } }
}

// Caller holds sched_mx.
static int8_t write_file_locked(void)
{
	char path[300], tmp[310];
	FILE *fp;
	int32_t i, k;

	snprintf(path, sizeof(path), "%stask.cfg", cs_confdir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if(!fp) { cs_log("cannot write %s (errno=%d %s)", tmp, errno, strerror(errno)); return 0; }

	fprintf(fp, "# OSCam task scheduler - managed by WebIF > Scheduler (can also be edited by hand,\n"
	            "# then save it from the task.cfg tab to apply it).\n\n[scheduler]\nenabled = %d\n", master_enabled ? 1 : 0);

	for(i = 0; i < job_count; i++)
	{
		const sched_job_t *j = &jobs[i];
		static const char *types[] = { "every", "daily", "weekly", "monthly", "once", "startup" };
		fprintf(fp, "\n[job:%s]\nenabled = %d\nschedule = %s\n", j->name, j->enabled ? 1 : 0, types[(int)j->stype]);
		switch(j->stype)
		{
			case SCHED_EVERY:   fprintf(fp, "every = %ds\n", j->every_sec); break;
			case SCHED_DAILY:
			case SCHED_WEEKLY:
			case SCHED_MONTHLY:
				fprintf(fp, "time = ");
				for(k = 0; k < j->ntimes; k++) { fprintf(fp, "%s%02d:%02d", k ? ", " : "", j->times[k] / 60, j->times[k] % 60); }
				fprintf(fp, "\n");
				if(j->stype == SCHED_WEEKLY)
				{
					int8_t first = 1;
					fprintf(fp, "days = ");
					for(k = 1; k <= 7; k++) { if((j->wdays >> (k % 7)) & 1) { fprintf(fp, "%s%s", first ? "" : ",", wday_names[k % 7]); first = 0; } }
					fprintf(fp, "\n");
				}
				if(j->stype == SCHED_MONTHLY) { fprintf(fp, "day = %d\n", j->mday); }
				break;
			case SCHED_ONCE:
			{
				struct tm tm;
				localtime_r(&j->once, &tm);
				fprintf(fp, "date = %04d-%02d-%02d %02d:%02d:%02d\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
				break;
			}
			case SCHED_STARTUP: fprintf(fp, "delay = %d\n", j->delay_sec); break;
		}
		for(k = 0; k < j->nsteps; k++)
		{
			if(j->steps[k].arg[0]) { fprintf(fp, "do = %s %s\n", j->steps[k].act, j->steps[k].arg); }
			else { fprintf(fp, "do = %s\n", j->steps[k].act); }
		}
	}
	if(fclose(fp) != 0) { cs_log("error writing %s", tmp); return 0; }
	if(rename(tmp, path) != 0) { cs_log("cannot replace %s (errno=%d %s)", path, errno, strerror(errno)); return 0; }
	return 1;
}

/* ------------------------------------------------------------- execution */

typedef struct
{
	char name[SCHED_NAME_LEN];
	int32_t nsteps;
	sched_step_t steps[SCHED_MAX_STEPS];
} run_ctx_t;

static void sleep_interruptible(int32_t seconds)
{
	int32_t i;
	for(i = 0; i < seconds * 10 && sched_running; i++) { cs_sleepms(100); }
}

static int8_t has_ext(const char *path, const char *ext)
{
	size_t lp = strlen(path), le = strlen(ext);
	return lp > le && !strcasecmp(path + lp - le, ext);
}

static int32_t run_script(const char *arg, char *msg, size_t msglen)
{
	char name[128], params[300] = "", cmd[600], buf[256];
	struct stat st;
	FILE *fp;
	int32_t rc;
	const char *sp;

	if(!cfg.http_script || !*cfg.http_script) { snprintf(msg, msglen, "no script directory configured (httpscript)"); return -1; }
	sp = arg;
	{
		size_t n = 0;
		while(*sp && !isspace((unsigned char)*sp) && n < sizeof(name) - 1) { name[n++] = *sp++; }
		name[n] = '\0';
		while(*sp && isspace((unsigned char)*sp)) { sp++; }
		cs_strncpy(params, sp, sizeof(params));
	}
	if(!*name || strchr(name, '/') || strstr(name, "..")) { snprintf(msg, msglen, "invalid script name"); return -1; }
	if(!has_ext(name, ".script") && !has_ext(name, ".sh")) { snprintf(msg, msglen, "script must end with .script or .sh"); return -1; }
	snprintf(cmd, sizeof(cmd), "%s/%s", cfg.http_script, name);
	if(stat(cmd, &st) || !S_ISREG(st.st_mode)) { snprintf(msg, msglen, "script '%s' not found", name); return -1; }
	if(!(st.st_mode & S_IXUSR)) { snprintf(msg, msglen, "script '%s' is not executable", name); return -1; }
	if(*params)
	{
		if(strpbrk(params, "\n\r")) { snprintf(msg, msglen, "invalid script parameters"); return -1; }
		cs_strncat(cmd, " ", sizeof(cmd));
		cs_strncat(cmd, params, sizeof(cmd));
	}
	fp = popen(cmd, "r");
	if(!fp) { snprintf(msg, msglen, "cannot start script '%s'", name); return -1; }
	while(fgets(buf, sizeof(buf), fp)) { trim(buf); if(*buf) { cs_log("script %s: %s", name, buf); } }
	rc = pclose(fp);
	rc = (rc == -1) ? -1 : (rc >> 8) & 0xff;
	if(rc) { snprintf(msg, msglen, "script '%s' returned %d", name, rc); return -1; }
	return 0;
}

static int32_t run_step(const char *job, const sched_step_t *s, char *msg, size_t msglen)
{
	msg[0] = '\0';
	if(!strcmp(s->act, "sleep")) { sleep_interruptible(atoi(s->arg)); return 0; }
	if(!strcmp(s->act, "log")) { cs_log("[%s] %s", job, s->arg); return 0; }
	if(!strcmp(s->act, "api")) { return sched_api_call(s->arg, msg, msglen); }
	if(!strcmp(s->act, "script.run")) { return run_script(s->arg, msg, msglen); }
	if(!sched_is_known_action(s->act)) { snprintf(msg, msglen, "unknown action '%s'", s->act); return -1; }
	return sched_exec_action(s->act, s->arg, msg, msglen);
}

static void *worker_main(void *arg)
{
	run_ctx_t *ctx = (run_ctx_t *)arg;
	int32_t i, ok = 1;
	char msg[SCHED_MSG_LEN], first_err[SCHED_MSG_LEN] = "";
	int32_t jidx;
	time_t now;

	cs_log("job '%s' started (%d step%s)", ctx->name, ctx->nsteps, ctx->nsteps == 1 ? "" : "s");
	for(i = 0; i < ctx->nsteps && sched_running; i++)
	{
		if(run_step(ctx->name, &ctx->steps[i], msg, sizeof(msg)) != 0)
		{
			cs_log("[%s] step %d (%s) failed: %s", ctx->name, i + 1, ctx->steps[i].act, msg);
			if(ok) { snprintf(first_err, sizeof(first_err), "step %d: %s", i + 1, msg); }
			ok = 0;
		}
	}
	cs_log("job '%s' finished: %s", ctx->name, ok ? "ok" : "with errors");

	now = cs_time();
	pthread_mutex_lock(&sched_mx);
	for(jidx = 0; jidx < job_count; jidx++)
	{
		if(!strcmp(jobs[jidx].name, ctx->name))
		{
			jobs[jidx].running = 0;
			jobs[jidx].last_run = now;
			jobs[jidx].run_count++;
			jobs[jidx].last_ok = ok;
			cs_strncpy(jobs[jidx].last_msg, ok ? "OK" : first_err, SCHED_MSG_LEN);
			break;
		}
	}
	workers--;
	pthread_mutex_unlock(&sched_mx);
	NULLFREE(ctx);
	return NULL;
}

// Caller holds sched_mx. Starts a worker for job j.
static void launch_locked(sched_job_t *j)
{
	run_ctx_t *ctx;
	pthread_t th;
	pthread_attr_t attr;

	if(j->running) { cs_log("job '%s' skipped: previous run still active", j->name); return; }
	if(!cs_malloc(&ctx, sizeof(run_ctx_t))) { return; }
	cs_strncpy(ctx->name, j->name, SCHED_NAME_LEN);
	ctx->nsteps = j->nsteps;
	memcpy(ctx->steps, j->steps, sizeof(j->steps));

	j->running = 1;
	workers++;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	if(pthread_create(&th, &attr, worker_main, ctx) != 0)
	{
		j->running = 0;
		workers--;
		cs_log("job '%s': cannot create thread", j->name);
		NULLFREE(ctx);
	}
	pthread_attr_destroy(&attr);
}

static void *scheduler_main(void *UNUSED(arg))
{
	while(sched_running)
	{
		time_t now = cs_time();
		int32_t i;

		pthread_mutex_lock(&sched_mx);
		if(master_enabled)
		{
			for(i = 0; i < job_count; i++)
			{
				sched_job_t *j = &jobs[i];
				if(!j->enabled || !j->next_run || j->next_run > now) { continue; }

				if(j->stype == SCHED_ONCE || j->stype == SCHED_STARTUP) { j->next_run = 0; j->done = 1; }
				else
				{
					// skip missed slots (e.g. after a long suspend) instead of running them in a burst
					j->next_run = compute_next(j, now);
				}
				launch_locked(j);
			}
		}
		pthread_mutex_unlock(&sched_mx);
		cs_sleepms(500);
	}
	return NULL;
}

/* ----------------------------------------------------------- public API */

// Caller holds sched_mx. Initial next_run for a freshly loaded/edited job.
static void init_next_locked(sched_job_t *j)
{
	time_t now = cs_time();
	j->done = 0;
	switch(j->stype)
	{
		case SCHED_STARTUP: j->next_run = start_time + j->delay_sec; if(j->next_run < now) { j->next_run = 0; j->done = 1; } break;
		case SCHED_ONCE:    j->next_run = j->once > now ? j->once : 0; if(!j->next_run) { j->done = 1; } break;
		default:            j->next_run = compute_next(j, now);
	}
}

static int8_t load_locked(void)
{
	sched_job_t *tmp = NULL;
	int32_t n, i, k;
	int8_t en = 1;

	if(!cs_malloc(&tmp, sizeof(sched_job_t) * SCHED_MAX_JOBS)) { return 0; }
	n = parse_file(tmp, &en);

	for(i = 0; i < n; i++)
	{
		sched_job_t *nj = &tmp[i];
		nj->id = next_id++;
		init_next_locked(nj);
		for(k = 0; k < job_count; k++) // keep run statistics of jobs that existed before
		{
			if(!strcmp(jobs[k].name, nj->name))
			{
				nj->last_run = jobs[k].last_run;
				nj->run_count = jobs[k].run_count;
				nj->last_ok = jobs[k].last_ok;
				cs_strncpy(nj->last_msg, jobs[k].last_msg, SCHED_MSG_LEN);
				nj->running = jobs[k].running;
				if(nj->stype == jobs[k].stype && (nj->stype == SCHED_STARTUP || nj->stype == SCHED_ONCE) && jobs[k].done)
					{ nj->done = 1; nj->next_run = 0; }
				break;
			}
		}
	}
	memcpy(jobs, tmp, sizeof(sched_job_t) * (size_t)n);
	job_count = n;
	master_enabled = en;
	NULLFREE(tmp);
	return 1;
}

void sched_init(void)
{
	start_time = cs_time();
	if(!cs_malloc(&jobs, sizeof(sched_job_t) * SCHED_MAX_JOBS)) { return; }

	pthread_mutex_lock(&sched_mx);
	load_locked();
	pthread_mutex_unlock(&sched_mx);

	sched_running = 1;
	if(pthread_create(&sched_thread, NULL, scheduler_main, NULL) != 0)
	{
		sched_running = 0;
		cs_log("cannot start scheduler thread");
		return;
	}
	sched_thread_started = 1;
	cs_log("scheduler started: %d job(s)%s", job_count, master_enabled ? "" : " (scheduler is switched off)");
}

void sched_shutdown(void)
{
	int32_t wait;
	if(!sched_thread_started) { return; }
	sched_running = 0;
	pthread_join(sched_thread, NULL);
	sched_thread_started = 0;
	for(wait = 0; wait < 50 && workers > 0; wait++) { cs_sleepms(100); }
	if(workers > 0) { cs_log("scheduler stopped with %d job(s) still running", workers); }
	else { cs_log("scheduler stopped"); }
}

int8_t sched_get_enabled(void)
{
	return master_enabled;
}

void sched_set_enabled(int8_t on)
{
	pthread_mutex_lock(&sched_mx);
	master_enabled = on ? 1 : 0;
	write_file_locked();
	pthread_mutex_unlock(&sched_mx);
}

int32_t sched_snapshot(sched_job_t *out, int32_t max)
{
	int32_t n;
	pthread_mutex_lock(&sched_mx);
	n = job_count < max ? job_count : max;
	if(n > 0 && jobs) { memcpy(out, jobs, sizeof(sched_job_t) * (size_t)n); }
	pthread_mutex_unlock(&sched_mx);
	return n;
}

static sched_job_t *find_locked(uint32_t id)
{
	int32_t i;
	for(i = 0; i < job_count; i++) { if(jobs[i].id == id) { return &jobs[i]; } }
	return NULL;
}

uint32_t sched_job_save(const sched_job_t *job, uint32_t id, char *err, size_t errlen)
{
	sched_job_t nj;
	sched_job_t *old;
	int32_t i;
	uint32_t rid = 0;
	char name[SCHED_NAME_LEN];

	if(!jobs) { snprintf(err, errlen, "scheduler not available"); return 0; }
	memcpy(&nj, job, sizeof(nj));
	clean_text(nj.name);
	cs_strncpy(name, nj.name, sizeof(name));
	trim(name);
	for(i = 0; name[i]; i++) { if(name[i] == ']' || name[i] == '[') { name[i] = '-'; } }
	cs_strncpy(nj.name, name, SCHED_NAME_LEN);
	if(!*nj.name) { snprintf(err, errlen, "job name is empty"); return 0; }
	for(i = 0; i < nj.nsteps; i++)
	{
		clean_text(nj.steps[i].arg);
		if(!sched_is_known_action(nj.steps[i].act)) { snprintf(err, errlen, "unknown action '%s'", nj.steps[i].act); return 0; }
	}
	if(nj.ntimes > 1) { qsort(nj.times, (size_t)nj.ntimes, sizeof(int16_t), cmp_i16); }
	{
		char e2[64] = "";
		if(!finalize_job(&nj, 1, 0, 0, 0, 0, 0, e2, sizeof(e2))) { snprintf(err, errlen, "%s", e2); return 0; }
	}

	pthread_mutex_lock(&sched_mx);
	old = id ? find_locked(id) : NULL;
	if(id && !old) { pthread_mutex_unlock(&sched_mx); snprintf(err, errlen, "job no longer exists"); return 0; }
	for(i = 0; i < job_count; i++)
	{
		if(&jobs[i] != old && !strcasecmp(jobs[i].name, nj.name))
			{ pthread_mutex_unlock(&sched_mx); snprintf(err, errlen, "a job named '%s' already exists", nj.name); return 0; }
	}
	if(!old && job_count >= SCHED_MAX_JOBS) { pthread_mutex_unlock(&sched_mx); snprintf(err, errlen, "job limit reached (%d)", SCHED_MAX_JOBS); return 0; }

	if(old)
	{
		nj.id = old->id;
		nj.last_run = old->last_run; nj.run_count = old->run_count; nj.last_ok = old->last_ok;
		cs_strncpy(nj.last_msg, old->last_msg, SCHED_MSG_LEN);
		nj.running = old->running;
		if(strcmp(old->name, nj.name) && old->running) { nj.running = 0; } // a renamed running job can't report back
		init_next_locked(&nj);
		*old = nj;
		rid = nj.id;
	}
	else
	{
		nj.id = next_id++;
		nj.last_run = 0; nj.run_count = 0; nj.running = 0; nj.last_msg[0] = '\0';
		init_next_locked(&nj);
		jobs[job_count++] = nj;
		rid = nj.id;
	}
	if(!write_file_locked()) { snprintf(err, errlen, "saved in memory, but task.cfg could not be written"); }
	pthread_mutex_unlock(&sched_mx);
	return rid;
}

int8_t sched_job_delete(uint32_t id)
{
	int32_t i;
	int8_t ok = 0;
	pthread_mutex_lock(&sched_mx);
	for(i = 0; i < job_count; i++)
	{
		if(jobs[i].id == id)
		{
			memmove(&jobs[i], &jobs[i + 1], sizeof(sched_job_t) * (size_t)(job_count - i - 1));
			job_count--;
			ok = 1;
			break;
		}
	}
	if(ok) { write_file_locked(); }
	pthread_mutex_unlock(&sched_mx);
	return ok;
}

int8_t sched_job_toggle(uint32_t id)
{
	sched_job_t *j;
	int8_t ok = 0;
	pthread_mutex_lock(&sched_mx);
	j = find_locked(id);
	if(j)
	{
		j->enabled = j->enabled ? 0 : 1;
		if(j->enabled && !j->done) { init_next_locked(j); }
		write_file_locked();
		ok = 1;
	}
	pthread_mutex_unlock(&sched_mx);
	return ok;
}

int8_t sched_job_run_now(uint32_t id)
{
	sched_job_t *j;
	int8_t ok = 0;
	pthread_mutex_lock(&sched_mx);
	j = find_locked(id);
	if(j && !j->running) { launch_locked(j); ok = 1; }
	pthread_mutex_unlock(&sched_mx);
	return ok;
}

int8_t sched_reload(void)
{
	int8_t ok;
	if(!jobs) { return 0; }
	pthread_mutex_lock(&sched_mx);
	ok = load_locked();
	pthread_mutex_unlock(&sched_mx);
	if(ok) { cs_log("task.cfg reloaded: %d job(s)", job_count); }
	return ok;
}

/* ------------------------------------------------------------ raw file */

#define SCHED_RAW_MAX 65535

char *sched_raw_read(void)
{
	char path[300], *buf;
	FILE *fp;
	long len;
	size_t n;

	snprintf(path, sizeof(path), "%stask.cfg", cs_confdir);
	fp = fopen(path, "r");
	if(!fp) { return strdup(""); }
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	rewind(fp);
	if(len < 0 || len > 1048576L || !(buf = malloc((size_t)len + 1))) { fclose(fp); return strdup(""); }
	n = fread(buf, 1, (size_t)len, fp);
	buf[n] = '\0';
	fclose(fp);
	return buf;
}

int8_t sched_raw_write(const char *text, char *err, size_t errlen)
{
	char path[300], tmp[310];
	FILE *fp;
	size_t len = strlen(text);
	int8_t ok;

	if(!jobs) { snprintf(err, errlen, "scheduler not available"); return 0; }
	if(len > SCHED_RAW_MAX) { snprintf(err, errlen, "task.cfg is too large (64 KB maximum)"); return 0; }
	snprintf(path, sizeof(path), "%stask.cfg", cs_confdir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	pthread_mutex_lock(&sched_mx);
	fp = fopen(tmp, "w");
	ok = fp && fwrite(text, 1, len, fp) == len;
	if(fp && fclose(fp) != 0) { ok = 0; }
	if(ok && rename(tmp, path) != 0) { ok = 0; }
	if(!ok) { remove(tmp); snprintf(err, errlen, "cannot write task.cfg (check the permissions of the config directory)"); }
	else { load_locked(); }
	pthread_mutex_unlock(&sched_mx);
	if(ok) { cs_log("task.cfg replaced from WebIF: %d job(s)", job_count); }
	return ok;
}

#endif // WEBIF
