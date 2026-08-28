#include "project.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int shell_in(const char *project, const char *command, bool timed) {
    struct timespec before, after; if (timed) clock_gettime(CLOCK_MONOTONIC, &before);
    pid_t pid = fork();
    if (pid == 0) { chdir(project); execl("/bin/sh", "sh", "-c", command, (char *)NULL); _exit(127); }
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (timed) {
        clock_gettime(CLOCK_MONOTONIC, &after);
        double elapsed = after.tv_sec - before.tv_sec + (after.tv_nsec - before.tv_nsec) / 1e9;
        printf("%s %s in %.2fs\n", code == 0 ? "✓" : "✗", code == 0 ? "built" : "build failed", elapsed);
    }
    return code;
}

int project_build(const char *build, const char *project) {
    const char *override = getenv("OVERKILL_BUILD");
    if (!override) override = getenv("CTXSH_BUILD");
    const char *command = override;
    if (!command || !*command) {
        if (!strcmp(build, "make")) command = "make";
        else if (!strcmp(build, "cmake")) command = "cmake --build build";
        else if (!strcmp(build, "cargo")) command = "cargo build";
        else if (!strcmp(build, "npm")) command = "npm run build";
        else if (!strcmp(build, "go")) command = "go build ./...";
        else if (!strcmp(build, "python")) command = "python -m build";
    }
    if (!command) { fprintf(stderr, "overkill: build: no build system detected (set OVERKILL_BUILD)\n"); return 1; }
    printf("%s\n", command); return shell_in(project, command, true);
}

int project_run(const char *build, const char *project) {
    const char *override = getenv("OVERKILL_RUN"); const char *command = override;
    if (!override) { override = getenv("CTXSH_RUN"); command = override; }
    char inferred[PATH_MAX];
    if (!command || !*command) {
        if (!strcmp(build, "cargo")) command = "cargo run";
        else if (!strcmp(build, "npm")) command = "npm start";
        else if (!strcmp(build, "go")) command = "go run .";
        else if (!strcmp(build, "python")) command = "python .";
        else if (!strcmp(build, "make")) {
            const char *base = strrchr(project, '/'); base = base ? base + 1 : project;
            snprintf(inferred, sizeof(inferred), "if [ -x ./bin/%s ]; then exec ./bin/%s; elif [ -x ./%s ]; then exec ./%s; else exec make run; fi", base, base, base, base);
            command = inferred;
        } else if (!strcmp(build, "cmake")) command = "ctest --test-dir build --output-on-failure";
    }
    if (!command) { fprintf(stderr, "overkill: run: cannot infer command (set OVERKILL_RUN)\n"); return 1; }
    printf("%s\n", command); return shell_in(project, command, false);
}

typedef struct { char ext[24]; size_t count; } Extension;

static bool ignored_dir(const char *name) {
    const char *ignored[] = {".git", ".ctx", "node_modules", "target", "build", ".venv", NULL};
    for (size_t i = 0; ignored[i]; i++) if (!strcmp(name, ignored[i])) return true;
    return false;
}

static void count_files(const char *dir, Extension *items, size_t *count) {
    DIR *d = opendir(dir); if (!d) return; struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st; if (lstat(path, &st) < 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) { if (!ignored_dir(entry->d_name)) count_files(path, items, count); continue; }
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot && strcmp(entry->d_name, "Makefile") && strcmp(entry->d_name, "CMakeLists.txt") && strcmp(entry->d_name, "Dockerfile")) continue;
        const char *ext = dot ? dot : entry->d_name;
        size_t i; for (i = 0; i < *count; i++) if (!strcmp(items[i].ext, ext)) break;
        if (i == *count && *count < 64) { snprintf(items[i].ext, sizeof(items[i].ext), "%s", ext); (*count)++; }
        if (i < 64) items[i].count++;
    }
    closedir(d);
}

int project_files(const char *project) {
    Extension items[64] = {{0}}; size_t count = 0; count_files(project, items, &count);
    for (size_t i = 0; i < count; i++) printf("%zu %-12s%s", items[i].count, items[i].ext, (i + 1) % 5 ? "" : "\n");
    if (count % 5) putchar('\n'); return 0;
}

static void scan_todos(const char *root, const char *dir, size_t *found) {
    DIR *d = opendir(dir); if (!d) return; struct dirent *entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st; if (lstat(path, &st) < 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) { if (!ignored_dir(entry->d_name)) scan_todos(root, path, found); continue; }
        const char *ext = strrchr(entry->d_name, '.');
        const char *source_exts[] = {".c", ".h", ".cc", ".cpp", ".hpp", ".rs", ".go", ".py", ".js", ".ts", ".java", ".sh", NULL};
        bool source = false; for (size_t i = 0; ext && source_exts[i]; i++) if (!strcmp(ext, source_exts[i])) source = true;
        if (!source || st.st_size > 2 * 1024 * 1024) continue;
        FILE *f = fopen(path, "r"); if (!f) continue;
        char *line = NULL; size_t cap = 0, number = 0;
        while (getline(&line, &cap, f) >= 0) { number++; char *tag = strstr(line, "TODO"); if (!tag) tag = strstr(line, "FIXME");
          bool comment = false;
          if (tag) { char *slash = strstr(line, "//"), *hash = strchr(line, '#'), *block = strstr(line, "/*");
            comment = (slash && slash < tag) || (hash && hash < tag) || (block && block < tag); }
          if (tag && comment) {
            line[strcspn(line, "\r\n")] = '\0'; const char *relative = path + strlen(root); if (*relative == '/') relative++;
            char location[PATH_MAX]; snprintf(location, sizeof(location), "%s:%zu", relative, number);
            printf("%-32s %s\n", location, line); (*found)++;
        }}
        free(line); fclose(f);
    }
    closedir(d);
}

int project_todo(const char *project) { size_t found = 0; scan_todos(project, project, &found); if (!found) puts("No TODO or FIXME comments found."); return 0; }

int project_ports(void) {
#ifdef __APPLE__
    return system("lsof -nP -iTCP -sTCP:LISTEN 2>/dev/null | awk 'NR==1 {print \"PORT   PID      PROCESS\"; next} {n=split($9,a,\":\"); printf \"%-6s %-8s %s\\n\",a[n],$2,$1}'");
#else
    return system("ss -ltnp 2>/dev/null");
#endif
}

int project_changed(const char *project) {
    char git[PATH_MAX]; struct stat st; snprintf(git, sizeof(git), "%s/.git", project);
    if (stat(git, &st) < 0) { puts("Not a Git repository."); return 0; }
    return shell_in(project, "git status --short", false);
}
