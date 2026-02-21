/*
 * tiniclaw-c — cron scheduler
 * Supports: cron expressions (every-minute default), at: one-shots,
 *           every: intervals.  State persisted to JSON file.
 * Single background thread polls all jobs.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <cjson/cJSON.h>
#include "gateway.h"
#include "util.h"

/* ── nc_cron_next_run ───────────────────────────────────────────── */

bool nc_cron_next_run(const char *expr, const char *tz,
                      int64_t after, int64_t *out) {
    if (!expr || !out) return false;
    (void)tz; /* timezone handling not implemented; uses local */

    /* every:<N><unit> */
    if (strncmp(expr, "every:", 6) == 0) {
        const char *s = expr + 6;
        char *end;
        long v = strtol(s, &end, 10);
        if (v <= 0) return false;
        int64_t secs = v;
        if (*end == 'm') secs = v * 60;
        else if (*end == 'h') secs = v * 3600;
        *out = after + secs;
        return true;
    }

    /* at:<ISO8601> one-shot */
    if (strncmp(expr, "at:", 3) == 0) {
        struct tm t = {0};
        if (strptime(expr + 3, "%Y-%m-%dT%H:%M:%S", &t)) {
            time_t ts = mktime(&t);
            if (ts > 0) { *out = (int64_t)ts; return true; }
        }
        return false;
    }

    /* cron expression — minimal: run every minute as safe default */
    *out = after + 60;
    return true;
}

/* ── Persistence helpers ─────────────────────────────────────────── */

static void cron_save(NcCronScheduler *s) {
    if (!s->jobs_path[0]) return;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s->jobs_count; i++) {
        NcCronJob *j = &s->jobs[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id",          (double)j->id);
        cJSON_AddStringToObject(o, "name",         j->name);
        cJSON_AddNumberToObject(o, "type",         (double)j->type);
        cJSON_AddStringToObject(o, "command",      j->command);
        cJSON_AddNumberToObject(o, "schedule_kind",(double)j->schedule_kind);
        cJSON_AddStringToObject(o, "cron_expr",    j->cron_expr);
        cJSON_AddNumberToObject(o, "at_timestamp", (double)j->at_timestamp);
        cJSON_AddNumberToObject(o, "every_ms",     (double)j->every_ms);
        cJSON_AddStringToObject(o, "timezone",     j->timezone);
        cJSON_AddBoolToObject  (o, "enabled",      j->enabled);
        cJSON_AddNumberToObject(o, "delivery_mode",(double)j->delivery_mode);
        cJSON_AddStringToObject(o, "delivery_channel", j->delivery_channel);
        cJSON_AddStringToObject(o, "delivery_to",  j->delivery_to);
        cJSON_AddNumberToObject(o, "last_run",     (double)j->last_run);
        cJSON_AddNumberToObject(o, "next_run",     (double)j->next_run);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_Print(arr);
    cJSON_Delete(arr);
    if (!txt) return;
    FILE *f = fopen(s->jobs_path, "w");
    if (f) { fputs(txt, f); fclose(f); }
    free(txt);
}

static void cron_load(NcCronScheduler *s) {
    if (!s->jobs_path[0]) return;
    FILE *f = fopen(s->jobs_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz <= 0) { fclose(f); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);
    cJSON *arr = cJSON_Parse(buf); free(buf);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return; }

    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        NcCronJob job = {0};
        cJSON *v;
#define GET_STR(f, key) do { v = cJSON_GetObjectItemCaseSensitive(o, key); \
    if (v && cJSON_IsString(v)) snprintf(job.f, sizeof job.f, "%s", v->valuestring); } while(0)
#define GET_NUM(f, key) do { v = cJSON_GetObjectItemCaseSensitive(o, key); \
    if (v && cJSON_IsNumber(v)) job.f = (__typeof__(job.f))v->valuedouble; } while(0)
        GET_NUM(id, "id");
        GET_STR(name, "name");
        GET_NUM(type, "type");
        GET_STR(command, "command");
        GET_NUM(schedule_kind, "schedule_kind");
        GET_STR(cron_expr, "cron_expr");
        GET_NUM(at_timestamp, "at_timestamp");
        GET_NUM(every_ms, "every_ms");
        GET_STR(timezone, "timezone");
        v = cJSON_GetObjectItemCaseSensitive(o, "enabled");
        if (v) job.enabled = cJSON_IsTrue(v);
        GET_NUM(delivery_mode, "delivery_mode");
        GET_STR(delivery_channel, "delivery_channel");
        GET_STR(delivery_to, "delivery_to");
        GET_NUM(last_run, "last_run");
        GET_NUM(next_run, "next_run");
#undef GET_STR
#undef GET_NUM
        nc_cron_add(s, &job);
    }
    cJSON_Delete(arr);
}

/* ── Scheduler thread ────────────────────────────────────────────── */

static const char *sched_expr(const NcCronJob *j) {
    switch (j->schedule_kind) {
    case NC_SCHED_CRON:  return j->cron_expr;
    case NC_SCHED_AT:    return NULL; /* handled by at_timestamp */
    case NC_SCHED_EVERY: {
        /* Reconstruct every: expression from every_ms */
        static __thread char buf[32];
        uint64_t ms = j->every_ms;
        if (ms % 3600000 == 0) snprintf(buf, sizeof buf, "every:%lluh", (unsigned long long)(ms/3600000));
        else if (ms % 60000 == 0) snprintf(buf, sizeof buf, "every:%llum", (unsigned long long)(ms/60000));
        else snprintf(buf, sizeof buf, "every:%llus", (unsigned long long)(ms/1000));
        return buf;
    }
    }
    return j->cron_expr;
}

static void run_job(NcCronScheduler *s, NcCronJob *j) {
    if (!j->enabled) return;

    char output[8192] = "";

    if (j->type == NC_JOB_SHELL && j->command[0]) {
        char cmd[2048];
        snprintf(cmd, sizeof cmd, "%s 2>&1", j->command);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            size_t n = fread(output, 1, sizeof output - 1, fp);
            output[n] = '\0';
            pclose(fp);
        }
    } else if (j->type == NC_JOB_AGENT && s->agent && j->command[0]) {
        /* Delegate to agent; response goes into output */
        /* nc_agent_turn not available here without arena — log and skip */
        snprintf(output, sizeof output, "(agent job: %s)", j->command);
    }

    if (output[0])
        fprintf(stderr, "[cron/%s] %s\n", j->name[0] ? j->name : "?", output);

    /* Update timestamps */
    int64_t now = (int64_t)time(NULL);
    j->last_run = now;

    /* Compute next_run */
    if (j->schedule_kind == NC_SCHED_AT) {
        j->next_run = 0; /* one-shot: don't reschedule */
        j->enabled  = false;
    } else {
        const char *expr = sched_expr(j);
        int64_t next = 0;
        if (expr && nc_cron_next_run(expr, j->timezone[0] ? j->timezone : NULL, now, &next))
            j->next_run = next;
        else
            j->next_run = now + 60;
    }
}

static void *scheduler_thread(void *arg) {
    NcCronScheduler *s = arg;
    while (s->running) {
        struct timespec ts = { .tv_sec = 1 };
        nanosleep(&ts, NULL);
        if (!s->running) break;

        int64_t now = (int64_t)time(NULL);
        bool dirty = false;
        for (size_t i = 0; i < s->jobs_count; i++) {
            NcCronJob *j = &s->jobs[i];
            if (!j->enabled) continue;
            if (j->next_run > 0 && j->next_run <= now) {
                run_job(s, j);
                dirty = true;
            }
        }
        if (dirty) cron_save(s);
    }
    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────── */

int nc_cron_init(NcCronScheduler *s, NcConfig *cfg, NcAgent *agent) {
    memset(s, 0, sizeof *s);
    s->agent = agent;
    s->jobs_cap = 16;
    s->jobs = calloc(s->jobs_cap, sizeof *s->jobs);
    if (!s->jobs) return -1;

    if (cfg && cfg->cron.jobs_path[0])
        snprintf(s->jobs_path, sizeof s->jobs_path, "%s", cfg->cron.jobs_path);

    if (s->jobs_path[0]) cron_load(s);
    return 0;
}

void nc_cron_deinit(NcCronScheduler *s) {
    nc_cron_stop(s);
    free(s->jobs);
    memset(s, 0, sizeof *s);
}

int nc_cron_start(NcCronScheduler *s) {
    s->running = true;
    if (pthread_create(&s->thread, NULL, scheduler_thread, s) != 0) {
        s->running = false;
        return -1;
    }
    return 0;
}

void nc_cron_stop(NcCronScheduler *s) {
    if (!s->running) return;
    s->running = false;
    pthread_join(s->thread, NULL);
}

int nc_cron_add(NcCronScheduler *s, const NcCronJob *job) {
    if (!job) return -1;

    /* Grow array if needed */
    if (s->jobs_count >= s->jobs_cap) {
        size_t new_cap = s->jobs_cap ? s->jobs_cap * 2 : 16;
        NcCronJob *nj = realloc(s->jobs, new_cap * sizeof *nj);
        if (!nj) return -1;
        s->jobs = nj;
        s->jobs_cap = new_cap;
    }

    NcCronJob *j = &s->jobs[s->jobs_count++];
    *j = *job;

    /* Assign ID if not set */
    if (j->id == 0) j->id = s->jobs_count;

    /* Compute initial next_run if not set */
    if (j->next_run == 0 && j->enabled) {
        int64_t now = (int64_t)time(NULL);
        if (j->schedule_kind == NC_SCHED_AT) {
            j->next_run = j->at_timestamp;
        } else {
            const char *expr = sched_expr(j);
            if (expr) nc_cron_next_run(expr, j->timezone[0] ? j->timezone : NULL, now, &j->next_run);
            if (!j->next_run) j->next_run = now + 60;
        }
    }

    cron_save(s);
    return 0;
}

int nc_cron_remove(NcCronScheduler *s, uint64_t job_id) {
    for (size_t i = 0; i < s->jobs_count; i++) {
        if (s->jobs[i].id == job_id) {
            memmove(&s->jobs[i], &s->jobs[i + 1],
                    sizeof(NcCronJob) * (s->jobs_count - i - 1));
            s->jobs_count--;
            cron_save(s);
            return 0;
        }
    }
    return -1;
}

int nc_cron_update(NcCronScheduler *s, uint64_t job_id, const NcCronJob *updated) {
    if (!updated) return -1;
    for (size_t i = 0; i < s->jobs_count; i++) {
        if (s->jobs[i].id == job_id) {
            s->jobs[i] = *updated;
            s->jobs[i].id = job_id; /* preserve original id */
            cron_save(s);
            return 0;
        }
    }
    return -1;
}

NcCronJob *nc_cron_list(NcCronScheduler *s, size_t *count_out) {
    if (count_out) *count_out = s->jobs_count;
    return s->jobs;
}

int nc_cron_cmd(NcConfig *cfg, int argc, const char **argv) {
    (void)cfg; (void)argc; (void)argv;
    fprintf(stderr, "tiniclaw cron: not implemented\n");
    return 1;
}
