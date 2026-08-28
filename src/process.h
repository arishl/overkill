#ifndef OVERKILL_PROCESS_H
#define OVERKILL_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_JOBS 64

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_EXITED } JobState;

typedef struct {
    int id;
    pid_t pid;
    char command[1024];
    char log_path[4096];
    long started;
    JobState state;
    int exit_status;
} ManagedJob;

typedef struct {
    ManagedJob items[MAX_JOBS];
    size_t count;
    int next_id;
} JobTable;

void jobs_init(JobTable *table);
void jobs_reap(JobTable *table);
size_t jobs_running(JobTable *table);
int jobs_start(JobTable *table, const char *command, const char *project);
int jobs_stop(JobTable *table, int id);
int jobs_restart(JobTable *table, int id, const char *project);
void jobs_print(JobTable *table);
void jobs_shutdown(JobTable *table);

#endif
