#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static ManagedJob *find_job(JobTable *table, int id) {
    for (size_t i = 0; i < table->count; i++) if (table->items[i].id == id) return &table->items[i];
    return NULL;
}

void jobs_init(JobTable *table) { memset(table, 0, sizeof(*table)); table->next_id = 1; }

void jobs_reap(JobTable *table) {
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < table->count; i++) if (table->items[i].pid == pid) {
            table->items[i].state = JOB_EXITED;
            table->items[i].exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
    }
}

size_t jobs_running(JobTable *table) {
    jobs_reap(table); size_t count = 0;
    for (size_t i = 0; i < table->count; i++) if (table->items[i].state == JOB_RUNNING) count++;
    return count;
}

static void make_log_dir(const char *project) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/.ctx", project); mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.ctx/logs", project); mkdir(path, 0755);
}

int jobs_start(JobTable *table, const char *command, const char *project) {
    if (table->count >= MAX_JOBS) { fprintf(stderr, "overkill: job table is full\n"); return 1; }
    make_log_dir(project);
    ManagedJob *job = &table->items[table->count];
    memset(job, 0, sizeof(*job)); job->id = table->next_id++;
    snprintf(job->command, sizeof(job->command), "%s", command);
    snprintf(job->log_path, sizeof(job->log_path), "%s/.ctx/logs/%d.log", project, job->id);
    int fd = open(job->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { fprintf(stderr, "overkill: start: %s\n", strerror(errno)); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0); chdir(project);
        dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL); _exit(127);
    }
    close(fd);
    if (pid < 0) { fprintf(stderr, "overkill: start: %s\n", strerror(errno)); return 1; }
    job->pid = pid; job->started = (long)time(NULL); job->state = JOB_RUNNING; table->count++;
    printf("[%d] %s\n    PID: %d\n    logs: %s\n", job->id, job->command, (int)pid, job->log_path);
    return 0;
}

int jobs_stop(JobTable *table, int id) {
    jobs_reap(table); ManagedJob *job = find_job(table, id);
    if (!job) { fprintf(stderr, "overkill: stop: no job %d\n", id); return 1; }
    if (job->state != JOB_RUNNING) { fprintf(stderr, "overkill: stop: job %d is not running\n", id); return 1; }
    if (kill(-job->pid, SIGTERM) < 0 && kill(job->pid, SIGTERM) < 0) { fprintf(stderr, "overkill: stop: %s\n", strerror(errno)); return 1; }
    job->state = JOB_STOPPED; printf("[%d] stopped %s\n", id, job->command); return 0;
}

int jobs_restart(JobTable *table, int id, const char *project) {
    ManagedJob *job = find_job(table, id);
    if (!job) { fprintf(stderr, "overkill: restart: no job %d\n", id); return 1; }
    char command[sizeof(job->command)]; snprintf(command, sizeof(command), "%s", job->command);
    if (job->state == JOB_RUNNING) jobs_stop(table, id);
    return jobs_start(table, command, project);
}

static void resource_usage(pid_t pid, char *cpu, size_t cpu_size, char *mem, size_t mem_size) {
    char command[128], output[128] = "";
    snprintf(command, sizeof(command), "ps -o %%cpu=,rss= -p %d 2>/dev/null", (int)pid);
    FILE *p = popen(command, "r");
    if (p && fgets(output, sizeof(output), p)) {
        double cpu_value = 0; long rss = 0;
        if (sscanf(output, "%lf %ld", &cpu_value, &rss) == 2) {
            snprintf(cpu, cpu_size, "%.1f%%", cpu_value); snprintf(mem, mem_size, "%.1fMB", rss / 1024.0);
        }
    }
    if (p) pclose(p);
}

void jobs_print(JobTable *table) {
    jobs_reap(table); long now = (long)time(NULL);
    puts("ID   PID      PROCESS                         CPU     MEM      STARTED   STATE");
    for (size_t i = 0; i < table->count; i++) {
        ManagedJob *j = &table->items[i]; char cpu[16] = "-", mem[16] = "-", age[32];
        if (j->state == JOB_RUNNING) resource_usage(j->pid, cpu, sizeof(cpu), mem, sizeof(mem));
        long seconds = now - j->started;
        if (seconds < 60) snprintf(age, sizeof(age), "%lds", seconds);
        else if (seconds < 3600) snprintf(age, sizeof(age), "%ldm", seconds / 60);
        else snprintf(age, sizeof(age), "%ldh", seconds / 3600);
        const char *state = j->state == JOB_RUNNING ? "running" : j->state == JOB_STOPPED ? "stopped" : "exited";
        printf("%-4d %-8d %-31.31s %-7s %-8s %-9s %s\n", j->id, (int)j->pid, j->command, cpu, mem, age, state);
    }
}

int jobs_logs(JobTable *table, int id, size_t lines) {
    ManagedJob *job = NULL;
    if (id > 0) job = find_job(table, id);
    else if (table->count) job = &table->items[table->count - 1];
    if (!job) { fprintf(stderr, "overkill: logs: no matching managed job\n"); return 1; }
    FILE *f = fopen(job->log_path, "r");
    if (!f) { fprintf(stderr, "overkill: logs: %s\n", strerror(errno)); return 1; }
    if (!lines) lines = 50;
    char **ring = calloc(lines, sizeof(*ring));
    if (!ring) { fclose(f); return 1; }
    char *line = NULL; size_t cap = 0, count = 0;
    while (getline(&line, &cap, f) >= 0) { free(ring[count % lines]); ring[count++ % lines] = strdup(line); }
    size_t shown = count < lines ? count : lines, start = count > lines ? count % lines : 0;
    printf("==> %s <==\n", job->log_path);
    for (size_t i = 0; i < shown; i++) fputs(ring[(start + i) % lines], stdout);
    for (size_t i = 0; i < lines; i++) free(ring[i]);
    free(ring); free(line); fclose(f); return 0;
}

void jobs_shutdown(JobTable *table) {
    for (size_t i = 0; i < table->count; i++) if (table->items[i].state == JOB_RUNNING) kill(-table->items[i].pid, SIGTERM);
    jobs_reap(table);
}
