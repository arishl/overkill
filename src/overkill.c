#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "editor.h"
#include "process.h"
#include "project.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char cwd[PATH_MAX];
    char project[PATH_MAX];
    char git_branch[128];
    char language[32];
    char build[32];
    bool git_dirty;
    bool venv;
    bool docker;
    bool is_project;
    char lima[64];
    bool in_vm;
    char vm_type[64];
} Context;

static volatile sig_atomic_t interrupted = 0;

static void on_sigint(int sig) {
    (void)sig;
    interrupted = 1;
    write(STDOUT_FILENO, "\n", 1);
}

static bool exists(const char *dir, const char *name) {
    char path[PATH_MAX];
    struct stat st;
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    return n > 0 && (size_t)n < sizeof(path) && stat(path, &st) == 0;
}

static bool project_marker(const char *dir) {
    const char *markers[] = {".git", "Makefile", "CMakeLists.txt", "Cargo.toml",
                             "package.json", "pyproject.toml", "go.mod", NULL};
    for (size_t i = 0; markers[i]; i++)
        if (exists(dir, markers[i])) return true;
    return false;
}

static bool has_cpp_source(const char *dir, int depth) {
    if (depth > 3) return false;
    DIR *d = opendir(dir); if (!d) return false; struct dirent *entry; bool found = false;
    while (!found && (entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !strcmp(entry->d_name, ".git") || !strcmp(entry->d_name, "build")) continue;
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st; if (lstat(path, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) found = has_cpp_source(path, depth + 1);
        else { const char *dot = strrchr(entry->d_name, '.'); found = dot && (!strcmp(dot, ".cpp") || !strcmp(dot, ".cc") || !strcmp(dot, ".cxx") || !strcmp(dot, ".hpp")); }
    }
    closedir(d); return found;
}

static bool workspace_container(const char *dir) {
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    const char *names[] = {"Dev", "Developer", "Projects", "Code", "src", "repos", NULL};
    for (size_t i = 0; names[i]; i++) if (!strcasecmp(base, names[i])) return true;
    const char *home = getenv("HOME");
    return home && !strcmp(dir, home);
}

static bool find_project(const char *cwd, char *out, size_t size) {
    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", cwd);
    while (true) {
        if (project_marker(cur) && !workspace_container(cur)) {
            snprintf(out, size, "%s", cur);
            return true;
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = '\0';
    }
    snprintf(out, size, "%s", cwd);
    return false;
}

static bool capture(char *const argv[], const char *dir, char *out, size_t size) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return false;
    pid_t pid = fork();
    if (pid == 0) {
        if (dir) chdir(dir);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    ssize_t used = 0, n;
    while (used < (ssize_t)size - 1 &&
           (n = read(pipefd[0], out + used, size - 1 - (size_t)used)) > 0) used += n;
    out[used] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (used > 0 && isspace((unsigned char)out[used - 1])) out[--used] = '\0';
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void detect_virtualization(bool *in_vm, char *type, size_t type_size) {
    static bool detected = false, cached_vm = false;
    static char cached_type[64] = "host";
    if (!detected) {
        detected = true;
        const char *override = getenv("OVERKILL_VM");
        if (override && *override) {
            cached_vm = strcasecmp(override, "no") && strcasecmp(override, "host") && strcmp(override, "0");
            snprintf(cached_type, sizeof(cached_type), "%s", cached_vm ? override : "host");
        } else if (getenv("LIMA_INSTANCE")) {
            cached_vm = true; snprintf(cached_type, sizeof(cached_type), "lima");
        } else if (getenv("WSL_INTEROP") || getenv("WSL_DISTRO_NAME")) {
            cached_vm = true; snprintf(cached_type, sizeof(cached_type), "wsl");
        } else if (access("/.dockerenv", F_OK) == 0 || getenv("container")) {
            cached_vm = true; snprintf(cached_type, sizeof(cached_type), "container");
#ifdef __linux__
        } else {
            char output[128]; char *argv[] = {"systemd-detect-virt", NULL};
            if (capture(argv, NULL, output, sizeof(output)) && strcmp(output, "none")) {
                cached_vm = true; snprintf(cached_type, sizeof(cached_type), "%s", output);
            }
#elif defined(__APPLE__)
        } else {
            char output[8192]; char *argv[] = {"system_profiler", "SPHardwareDataType", NULL};
            if (capture(argv, NULL, output, sizeof(output)) &&
                (strstr(output, "Virtual Machine") || strstr(output, "VMware") ||
                 strstr(output, "Parallels") || strstr(output, "QEMU"))) {
                cached_vm = true; snprintf(cached_type, sizeof(cached_type), "virtual machine");
            }
#endif
        }
    }
    *in_vm = cached_vm; snprintf(type, type_size, "%s", cached_type);
}

static void detect_context(Context *c) {
    memset(c, 0, sizeof(*c));
    if (!getcwd(c->cwd, sizeof(c->cwd))) snprintf(c->cwd, sizeof(c->cwd), "?");
    detect_virtualization(&c->in_vm, c->vm_type, sizeof(c->vm_type));
    c->is_project = find_project(c->cwd, c->project, sizeof(c->project));

    char *git_root[] = {"git", "rev-parse", "--show-toplevel", NULL};
    char root[PATH_MAX];
    if (capture(git_root, c->cwd, root, sizeof(root))) {
        c->is_project = true;
        snprintf(c->project, sizeof(c->project), "%s", root);
        char *branch[] = {"git", "branch", "--show-current", NULL};
        if (!capture(branch, c->cwd, c->git_branch, sizeof(c->git_branch)) || !c->git_branch[0])
            snprintf(c->git_branch, sizeof(c->git_branch), "detached");
        char status[8];
        char *dirty[] = {"git", "status", "--porcelain", NULL};
        c->git_dirty = capture(dirty, c->cwd, status, sizeof(status)) && status[0];
    }

    if (!c->is_project) return;
    if (exists(c->project, "Cargo.toml")) snprintf(c->language, sizeof(c->language), "Rust");
    else if (exists(c->project, "go.mod")) snprintf(c->language, sizeof(c->language), "Go");
    else if (exists(c->project, "package.json")) snprintf(c->language, sizeof(c->language), "JS/TS");
    else if (exists(c->project, "pyproject.toml") || exists(c->project, "requirements.txt")) snprintf(c->language, sizeof(c->language), "Python");
    else if (exists(c->project, "CMakeLists.txt") || exists(c->project, "Makefile")) snprintf(c->language, sizeof(c->language), "%s", has_cpp_source(c->project, 0) ? "C++" : "C");

    if (exists(c->project, "CMakeLists.txt")) snprintf(c->build, sizeof(c->build), "cmake");
    else if (exists(c->project, "Makefile")) snprintf(c->build, sizeof(c->build), "make");
    else if (exists(c->project, "Cargo.toml")) snprintf(c->build, sizeof(c->build), "cargo");
    else if (exists(c->project, "package.json")) snprintf(c->build, sizeof(c->build), "npm");
    else if (exists(c->project, "go.mod")) snprintf(c->build, sizeof(c->build), "go");
    else if (exists(c->project, "pyproject.toml")) snprintf(c->build, sizeof(c->build), "python");

    c->venv = getenv("VIRTUAL_ENV") || exists(c->project, ".venv");
    c->docker = exists(c->project, "compose.yml") || exists(c->project, "compose.yaml") ||
                exists(c->project, "docker-compose.yml") || exists(c->project, "Dockerfile");
    const char *lima = getenv("OVERKILL_LIMA");
    if (!lima) lima = getenv("CTXSH_LIMA");
    if (!lima) lima = getenv("LIMA_INSTANCE");
    if (lima) snprintf(c->lima, sizeof(c->lima), "%s", lima);
    else if (exists(c->project, ".lima")) {
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/.lima", c->project);
        FILE *f = fopen(path, "r");
        if (f) { if (fgets(c->lima, sizeof(c->lima), f)) c->lima[strcspn(c->lima, "\r\n")] = '\0'; fclose(f); }
    }
}

static const char *display_path(const char *path, char *buf, size_t size) {
    const char *home = getenv("HOME");
    if (home && strncmp(path, home, strlen(home)) == 0 &&
        (path[strlen(home)] == '/' || path[strlen(home)] == '\0')) {
        snprintf(buf, size, "~%s", path + strlen(home));
        return buf;
    }
    return path;
}

static size_t terminal_width(void) {
    struct winsize ws;
    size_t width = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col ? ws.ws_col : 80;
    if (width < 44) width = 44;
    if (width > 140) width = 140;
    return width;
}

static void prompt_segment(const char *label, const char *value, const char *value_color,
                           bool color, bool *first, size_t *used) {
    const char *dim = color ? "\033[2m" : "", *reset = color ? "\033[0m" : "";
    if (!*first) { printf("%s  ·  %s", dim, reset); *used += 5; }
    printf("%s%s%s %s%s%s", dim, label, reset, color ? value_color : "", value, reset);
    *used += strlen(label) + 1 + strlen(value); *first = false;
}

static void print_context(const Context *c, bool color, size_t job_count, int last_status) {
    char path_buffer[PATH_MAX], short_path[PATH_MAX], jobs_value[32];
    const char *path = display_path(c->cwd, path_buffer, sizeof(path_buffer));
    const char *dim = color ? "\033[2m" : "", *cyan = color ? "\033[1;36m" : "";
    const char *green = color ? "\033[1;32m" : "", *yellow = color ? "\033[1;33m" : "";
    const char *magenta = color ? "\033[1;35m" : "", *red = color ? "\033[1;31m" : "";
    const char *reset = color ? "\033[0m" : "";
    size_t width = terminal_width(), path_limit = width > 12 ? width - 12 : 32;
    if (strlen(path) > path_limit) {
        const char *tail = path + strlen(path) - (path_limit - 2);
        snprintf(short_path, sizeof(short_path), "…%s", tail); path = short_path;
    }
    size_t path_len = strlen(path), fill = width > path_len + 5 ? width - path_len - 5 : 2;
    printf("%s╭─%s %s%s%s ", dim, reset, cyan, path, reset);
    for (size_t i = 0; i < fill; i++) fputs("─", stdout);
    printf("%s╮%s\n%s│%s  ", dim, reset, dim, reset);

    bool first = true; size_t used = 3;
    if (c->git_branch[0]) {
        char git[160]; snprintf(git, sizeof(git), "%s %s", c->git_branch, c->git_dirty ? "●" : "✓");
        prompt_segment("git", git, c->git_dirty ? yellow : green, color, &first, &used);
    }
    if (c->language[0]) prompt_segment("lang", c->language, cyan, color, &first, &used);
    if (c->build[0]) prompt_segment("build", c->build, magenta, color, &first, &used);
    if (c->venv) prompt_segment("venv", getenv("VIRTUAL_ENV") ? "active" : "ready", green, color, &first, &used);
    if (c->docker) prompt_segment("docker", "ready", cyan, color, &first, &used);
    if (c->lima[0]) prompt_segment("lima", c->lima, magenta, color, &first, &used);
    char vm_value[96];
    snprintf(vm_value, sizeof(vm_value), "%s%s%s", c->in_vm ? "yes (" : "no", c->in_vm ? c->vm_type : "", c->in_vm ? ")" : "");
    prompt_segment("vm", vm_value, c->in_vm ? yellow : green, color, &first, &used);
    snprintf(jobs_value, sizeof(jobs_value), "%zu", job_count);
    prompt_segment("jobs", jobs_value, job_count ? yellow : green, color, &first, &used);
    if (used + 1 < width) for (size_t i = used; i + 1 < width; i++) putchar(' ');
    printf("%s│%s\n%s╰─%s", dim, reset, dim, reset);
    if (last_status) printf("%s[%d]%s─", red, last_status, reset);
    printf("%s❯%s ", last_status ? red : green, reset);
    fflush(stdout);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static int change_dir(char *arg, char *previous, size_t size) {
    arg = trim(arg);
    const char *target = *arg ? arg : getenv("HOME");
    if (!target) target = "/";
    if (!strcmp(target, "-")) target = previous[0] ? previous : ".";
    char expanded[PATH_MAX];
    if (target[0] == '~' && (target[1] == '/' || target[1] == '\0')) {
        const char *home = getenv("HOME");
        snprintf(expanded, sizeof(expanded), "%s%s", home ? home : "", target + 1);
    }
    else snprintf(expanded, sizeof(expanded), "%s", target);
    char old[PATH_MAX];
    if (!getcwd(old, sizeof(old))) old[0] = '\0';
    if (chdir(expanded) < 0) { fprintf(stderr, "overkill: cd: %s: %s\n", expanded, strerror(errno)); return 1; }
    snprintf(previous, size, "%s", old);
    return 0;
}

static int run_command(const char *line) {
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        execl("/bin/sh", "sh", "-c", line, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void sanitize_environment(void) {
    const char *value = getenv("LSCOLORS");
    if (!value) return;
    bool valid = strlen(value) == 22;
    for (const char *p = value; valid && *p; p++)
        valid = *p == 'x' || (*p >= 'a' && *p <= 'h') || (*p >= 'A' && *p <= 'H');
    if (!valid) {
        if (getenv("OVERKILL_DEBUG") || getenv("CTXSH_DEBUG"))
            fprintf(stderr, "overkill: ignoring invalid LSCOLORS value '%s'\n", value);
        unsetenv("LSCOLORS");
    }
}

static void unquote(char *value) {
    size_t n = strlen(value);
    if (n >= 2 && ((value[0] == '"' && value[n - 1] == '"') ||
                   (value[0] == '\'' && value[n - 1] == '\''))) {
        memmove(value, value + 1, n - 2); value[n - 2] = '\0';
    }
}

static void load_config(const char *path, bool hooks, char *hook, size_t hook_size) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        char *entry = trim(line);
        if (!*entry || *entry == '#') continue;
        if (!strncmp(entry, "export ", 7)) entry = trim(entry + 7);
        if (!strncmp(entry, "unset ", 6)) { unsetenv(trim(entry + 6)); continue; }
        if (!strncmp(entry, "on_enter=", 9)) {
            if (hooks) { snprintf(hook, hook_size, "%s", trim(entry + 9)); unquote(hook); }
            continue;
        }
        char *equals = strchr(entry, '=');
        if (!equals) { fprintf(stderr, "overkill: %s: ignored invalid config line\n", path); continue; }
        *equals++ = '\0';
        char *name = trim(entry), *value = trim(equals); unquote(value);
        bool name_ok = (*name == '_' || isalpha((unsigned char)*name));
        for (char *p = name + 1; name_ok && *p; p++) name_ok = *p == '_' || isalnum((unsigned char)*p);
        if (name_ok) {
            if (!strcmp(name, "run")) setenv("OVERKILL_RUN", value, 1);
            else if (!strcmp(name, "build")) setenv("OVERKILL_BUILD", value, 1);
            else if (!strcmp(name, "lima")) setenv("OVERKILL_LIMA", value, 1);
            else setenv(name, value, 1);
        }
    }
    free(line); fclose(f);
}

static bool project_trusted(const char *project) {
    const char *home = getenv("HOME"); if (!home) return false;
    const char *names[] = {".overkill_trusted", ".ctxsh_trusted", NULL};
    bool found = false;
    for (size_t i = 0; names[i] && !found; i++) {
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", home, names[i]);
        FILE *f = fopen(path, "r"); if (!f) continue;
        char *line = NULL; size_t cap = 0;
        while (getline(&line, &cap, f) >= 0) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!strcmp(line, project)) { found = true; break; }
        }
        free(line); fclose(f);
    }
    return found;
}

static int trust_project(const Context *context) {
    if (!context->is_project) { fprintf(stderr, "overkill: trust: current directory is not a project\n"); return 1; }
    if (project_trusted(context->project)) { puts("overkill: project is already trusted"); return 0; }
    const char *home = getenv("HOME"); if (!home) return 1;
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/.overkill_trusted", home);
    FILE *f = fopen(path, "a");
    if (!f) { fprintf(stderr, "overkill: trust: %s\n", strerror(errno)); return 1; }
    fprintf(f, "%s\n", context->project); fclose(f);
    printf("overkill: trusted %s\n", context->project); return 0;
}

static unsigned long state_hash(const char *text_value) {
    unsigned long hash = 1469598103934665603UL;
    for (const unsigned char *p = (const unsigned char *)text_value; *p; p++) { hash ^= *p; hash *= 1099511628211UL; }
    return hash;
}

static bool state_paths(const char *project, char *state, size_t state_size, char *last, size_t last_size) {
    const char *home = getenv("HOME"); if (!home) return false;
    char dir[PATH_MAX]; snprintf(dir, sizeof(dir), "%s/.overkill_state", home); mkdir(dir, 0700);
    snprintf(state, state_size, "%s/%lx", dir, state_hash(project));
    snprintf(last, last_size, "%s/last", dir); return true;
}

static void state_record(const Context *context, const char *command) {
    if (!context->is_project || !*command) return;
    char state[PATH_MAX], last[PATH_MAX]; if (!state_paths(context->project, state, sizeof(state), last, sizeof(last))) return;
    char commands[3][4096] = {{0}}; size_t count = 0; FILE *old = fopen(state, "r");
    if (old) { char *line = NULL; size_t cap = 0; while (getline(&line, &cap, old) >= 0) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "cmd=", 4)) { if (count == 3) { memcpy(commands[0], commands[1], sizeof(commands[0])); memcpy(commands[1], commands[2], sizeof(commands[1])); count = 2; } snprintf(commands[count++], sizeof(commands[0]), "%s", line + 4); }
    } free(line); fclose(old); }
    if (count == 3) { memcpy(commands[0], commands[1], sizeof(commands[0])); memcpy(commands[1], commands[2], sizeof(commands[1])); count = 2; }
    snprintf(commands[count++], sizeof(commands[0]), "%s", command);
    FILE *f = fopen(state, "w");
    if (f) { fprintf(f, "project=%s\ncwd=%s\n", context->project, context->cwd); for (size_t i = 0; i < count; i++) fprintf(f, "cmd=%s\n", commands[i]); fclose(f); }
    f = fopen(last, "w"); if (f) { fprintf(f, "%s\n", state); fclose(f); }
}

static void state_welcome(const char *project) {
    char state[PATH_MAX], last[PATH_MAX]; if (!state_paths(project, state, sizeof(state), last, sizeof(last))) return;
    FILE *f = fopen(state, "r");
    if (!f) {
        const char *home = getenv("HOME");
        if (home) { snprintf(state, sizeof(state), "%s/.ctxsh_state/%lx", home, state_hash(project)); f = fopen(state, "r"); }
    }
    if (!f) return;
    char *line = NULL; size_t cap = 0; bool heading = false;
    while (getline(&line, &cap, f) >= 0) if (!strncmp(line, "cmd=", 4)) {
        if (!heading) {
            struct passwd *account = getpwuid(getuid());
            const char *user = account && account->pw_name ? account->pw_name : getenv("USER");
            printf("Welcome back%s%s.\n\nLast commands:\n", user ? ", " : "", user ? user : "");
            heading = true;
        }
        printf("  %s", line + 4);
    }
    free(line); fclose(f);
    if (heading && exists(project, ".git")) { puts("\nGit:"); project_changed(project); }
}

static int state_resume(void) {
    const char *home = getenv("HOME"); if (!home) return 1;
    char last[PATH_MAX]; snprintf(last, sizeof(last), "%s/.overkill_state/last", home);
    FILE *f = fopen(last, "r");
    if (!f) { snprintf(last, sizeof(last), "%s/.ctxsh_state/last", home); f = fopen(last, "r"); }
    if (!f) { fprintf(stderr, "overkill: resume: no saved project\n"); return 1; }
    char state[PATH_MAX]; if (!fgets(state, sizeof(state), f)) { fclose(f); return 1; } fclose(f); state[strcspn(state, "\r\n")] = '\0';
    f = fopen(state, "r"); if (!f) return 1; char *line = NULL; size_t cap = 0; char cwd[PATH_MAX] = "";
    while (getline(&line, &cap, f) >= 0) if (!strncmp(line, "cwd=", 4)) { snprintf(cwd, sizeof(cwd), "%s", line + 4); cwd[strcspn(cwd, "\r\n")] = '\0'; }
    free(line); fclose(f);
    if (!*cwd || chdir(cwd) < 0) { fprintf(stderr, "overkill: resume: %s\n", strerror(errno)); return 1; }
    return 0;
}

static void enter_project(const Context *context, char *active, size_t active_size, bool announce) {
    const char *next = context->is_project ? context->project : "";
    if (!strcmp(active, next)) return;
    snprintf(active, active_size, "%s", next);
    if (!*next) return;
    if (announce) state_welcome(next);
    char config[PATH_MAX]; snprintf(config, sizeof(config), "%s/.overkillrc", next);
    if (!exists(next, ".overkillrc")) {
        if (!exists(next, ".ctxshrc")) return;
        snprintf(config, sizeof(config), "%s/.ctxshrc", next);
    }
    if (!project_trusted(next)) {
        fprintf(stderr, "overkill: project config found; run 'trust' to enable %s\n", config);
        return;
    }
    char hook[4096] = "";
    load_config(config, true, hook, sizeof(hook));
    if (*hook) run_command(hook);
}

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    const char *details;
} HelpEntry;

static const HelpEntry help_entries[] = {
    {"cd", "cd [directory|-]", "Change directory", "With no directory, changes to $HOME. A dash returns to the previous directory."},
    {"resume", "resume", "Restore the last project directory", "Returns to the most recently saved project and subdirectory without rerunning commands."},
    {"context", "context", "Print the current project panel", "Shows Git, language, build system, environment, VM, Lima, and managed-job state."},
    {"build", "build", "Build the detected project", "Runs make, CMake, Cargo, npm, Go, or Python build logic. Override with build= in .overkillrc."},
    {"run", "run", "Run the detected project", "Infers the project run command. Override with run= in a trusted project .overkillrc."},
    {"files", "files", "Summarize project files", "Counts source files recursively by extension while excluding generated and vendor directories."},
    {"todo", "todo", "Find TODO and FIXME comments", "Prints source location, line number, and matching comment."},
    {"changed", "changed", "Show changed Git files", "Equivalent to a concise git status for the current project."},
    {"ports", "ports", "Show listening TCP ports", "Uses lsof on macOS and ss on Linux to show port, PID, and process."},
    {"start", "start <command>", "Launch a managed process", "Runs in its own process group and logs to .ctx/logs/<id>.log."},
    {"jobs", "jobs", "List managed processes", "Shows job ID, PID, CPU, memory, age, and lifecycle state."},
    {"stop", "stop <job-id>", "Stop a managed process", "Sends SIGTERM to the job's entire process group."},
    {"restart", "restart <job-id>", "Restart a managed process", "Stops the selected job if needed and relaunches its saved command."},
    {"history", "history [--full|count]", "Show persistent command history", "Without a count, prints the complete numbered history from ~/.overkill_history."},
    {"trust", "trust", "Trust project configuration", "Allows the current project's reviewed .overkillrc environment and on_enter hook."},
    {"help", "help [command]", "Show help", "Displays the command menu or detailed help for one built-in command."},
    {"exit", "exit [status]", "Exit Overkill", "Stops managed processes and exits with the optional numeric status."},
    {NULL, NULL, NULL, NULL}
};

static const HelpEntry *find_help(const char *name) {
    for (size_t i = 0; help_entries[i].name; i++) if (!strcmp(help_entries[i].name, name)) return &help_entries[i];
    return NULL;
}

static int print_help(const char *command, bool color) {
    const char *bold = color ? "\033[1m" : "", *cyan = color ? "\033[36m" : "";
    const char *dim = color ? "\033[2m" : "", *reset = color ? "\033[0m" : "";
    if (command && *command) {
        const HelpEntry *entry = find_help(command);
        if (!entry) { fprintf(stderr, "overkill: help: unknown command '%s'\n", command); return 1; }
        printf("%s%s%s — %s\n\n", bold, entry->usage, reset, entry->summary);
        printf("  %s\n", entry->details);
        return 0;
    }
    printf("%sOverkill%s  %scontext-aware developer shell%s\n\n", bold, reset, dim, reset);
    puts("NAVIGATION");
    for (size_t i = 0; i < 2; i++) printf("  %s%-26s%s %s\n", cyan, help_entries[i].usage, reset, help_entries[i].summary);
    puts("\nPROJECT");
    for (size_t i = 2; i < 9; i++) printf("  %s%-26s%s %s\n", cyan, help_entries[i].usage, reset, help_entries[i].summary);
    puts("\nPROCESSES");
    for (size_t i = 9; i < 13; i++) printf("  %s%-26s%s %s\n", cyan, help_entries[i].usage, reset, help_entries[i].summary);
    puts("\nSHELL");
    for (size_t i = 13; help_entries[i].name; i++) printf("  %s%-26s%s %s\n", cyan, help_entries[i].usage, reset, help_entries[i].summary);
    printf("\n%sAny other input is executed by /bin/sh, including pipes and redirects.%s\n", dim, reset);
    printf("%sConfig: ~/.overkillrc and trusted project .overkillrc files.%s\n", dim, reset);
    printf("%sTry 'help <command>' for details.%s\n", dim, reset);
    return 0;
}

int main(int argc, char **argv) {
    sanitize_environment();
    if (argc > 2 && !strcmp(argv[1], "-c")) return run_command(argv[2]);
    if (argc != 1) { fprintf(stderr, "usage: overkill [-c command]\n"); return 2; }
    bool interactive = isatty(STDIN_FILENO);
    const char *home = getenv("HOME");
    if (home) {
        char config[PATH_MAX]; snprintf(config, sizeof(config), "%s/.overkillrc", home);
        if (!exists(home, ".overkillrc") && exists(home, ".ctxshrc")) snprintf(config, sizeof(config), "%s/.ctxshrc", home);
        char hook[1]; load_config(config, false, hook, sizeof(hook));
    }
    sanitize_environment();
    signal(SIGINT, on_sigint);
    char *line = NULL;
    size_t capacity = 0;
    char previous[PATH_MAX] = "";
    char active_project[PATH_MAX] = "";
    History history;
    history_init(&history);
    JobTable jobs;
    jobs_init(&jobs);
    int last_status = 0;
    while (true) {
        Context context;
        detect_context(&context);
        jobs_reap(&jobs);
        enter_project(&context, active_project, sizeof(active_project), interactive);
        if (interactive) print_context(&context, isatty(STDOUT_FILENO) && !getenv("NO_COLOR"), jobs_running(&jobs), last_status);
        errno = 0;
        if (interactive) {
            free(line); line = editor_readline(&history);
            if (!line) { putchar('\n'); break; }
        } else {
            ssize_t n = getline(&line, &capacity, stdin);
            if (n < 0) break;
        }
        if (interrupted) { interrupted = 0; continue; }
        char *cmd = trim(line);
        if (!*cmd) continue;
        if (interactive) history_add(&history, cmd);
        if (interactive && strcmp(cmd, "resume")) state_record(&context, cmd);
        if (!strcmp(cmd, "exit")) break;
        if (!strncmp(cmd, "exit ", 5)) { last_status = atoi(trim(cmd + 5)); break; }
        if (!strcmp(cmd, "context")) { print_context(&context, false, jobs_running(&jobs), last_status); putchar('\n'); continue; }
        if (!strcmp(cmd, "help")) { last_status = print_help(NULL, interactive && isatty(STDOUT_FILENO) && !getenv("NO_COLOR")); continue; }
        if (!strncmp(cmd, "help ", 5)) { last_status = print_help(trim(cmd + 5), interactive && isatty(STDOUT_FILENO) && !getenv("NO_COLOR")); continue; }
        if (!strcmp(cmd, "history") || !strcmp(cmd, "history --full")) { history_print(&history, 0); last_status = 0; continue; }
        if (!strncmp(cmd, "history ", 8)) {
            char *amount = trim(cmd + 8); char *end = NULL; errno = 0; unsigned long limit = strtoul(amount, &end, 10);
            if (errno || !*amount || *end || limit == 0) { fprintf(stderr, "overkill: history: expected a positive count or --full\n"); last_status = 2; }
            else { history_print(&history, (size_t)limit); last_status = 0; }
            continue;
        }
        if (!strcmp(cmd, "trust")) { last_status = trust_project(&context); active_project[0] = '\0'; continue; }
        if (!strcmp(cmd, "build")) { last_status = project_build(context.build, context.project); continue; }
        if (!strcmp(cmd, "run")) { last_status = project_run(context.build, context.project); continue; }
        if (!strcmp(cmd, "files")) { last_status = project_files(context.project); continue; }
        if (!strcmp(cmd, "todo")) { last_status = project_todo(context.project); continue; }
        if (!strcmp(cmd, "ports")) { last_status = project_ports(); continue; }
        if (!strcmp(cmd, "changed")) { last_status = project_changed(context.project); continue; }
        if (!strcmp(cmd, "resume")) { last_status = state_resume(); active_project[0] = '\0'; continue; }
        if (!strcmp(cmd, "jobs")) { jobs_print(&jobs); last_status = 0; continue; }
        if (!strncmp(cmd, "start ", 6)) { last_status = jobs_start(&jobs, trim(cmd + 6), context.project); continue; }
        if (!strncmp(cmd, "stop ", 5)) { last_status = jobs_stop(&jobs, atoi(trim(cmd + 5))); continue; }
        if (!strncmp(cmd, "restart ", 8)) { last_status = jobs_restart(&jobs, atoi(trim(cmd + 8)), context.project); continue; }
        if (!strncmp(cmd, "cd", 2) && (cmd[2] == '\0' || isspace((unsigned char)cmd[2])))
            last_status = change_dir(cmd + 2, previous, sizeof(previous));
        else last_status = run_command(cmd);
    }
    free(line);
    history_free(&history);
    jobs_shutdown(&jobs);
    return last_status;
}
