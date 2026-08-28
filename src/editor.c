#include "editor.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define LINE_MAX_SIZE 4096
"Hello"
static void history_push(History *h, const char *line) {
    if (!*line) return;
    if (h->count == h->capacity) {
        size_t next = h->capacity ? h->capacity * 2 : 64;
        char **items = realloc(h->items, next * sizeof(*items));
        if (!items) return;
        h->items = items; h->capacity = next;
    }
    h->items[h->count++] = strdup(line);
}

void history_init(History *h) {
    memset(h, 0, sizeof(*h));
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(h->path, sizeof(h->path), "%s/.overkill_history", home);
    FILE *f = fopen(h->path, "r");
    if (!f) {
        char legacy[4096]; snprintf(legacy, sizeof(legacy), "%s/.ctxsh_history", home);
        f = fopen(legacy, "r");
    }
    if (!f) return;
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        line[strcspn(line, "\r\n")] = '\0';
        history_push(h, line);
    }
    free(line); fclose(f);
}

void history_add(History *h, const char *line) {
    size_t before = h->count;
    history_push(h, line);
    if (h->count == before || !h->path[0]) return;
    FILE *f = fopen(h->path, "a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

void history_print(const History *h, size_t limit) {
    size_t start = limit && limit < h->count ? h->count - limit : 0;
    for (size_t i = start; i < h->count; i++) printf("%6zu  %s\n", i + 1, h->items[i]);
}

void history_free(History *h) {
    for (size_t i = 0; i < h->count; i++) free(h->items[i]);
    free(h->items);
}

static void redraw(const char *buf, size_t len, size_t cursor) {
    (void)len;
    printf("\r\033[2K╰─❯ %s", buf);
    if (len > cursor) printf("\033[%zuD", len - cursor);
    fflush(stdout);
}

static void replace_line(char *buf, size_t *len, size_t *cursor, const char *value) {
    snprintf(buf, LINE_MAX_SIZE, "%s", value ? value : "");
    *len = *cursor = strlen(buf);
    redraw(buf, *len, *cursor);
}

static void complete_path(char *buf, size_t *len, size_t *cursor) {
    size_t start = *cursor;
    while (start && !isspace((unsigned char)buf[start - 1])) start--;
    char token[LINE_MAX_SIZE];
    size_t token_len = *cursor - start;
    memcpy(token, buf + start, token_len); token[token_len] = '\0';
    char dir[LINE_MAX_SIZE] = ".", prefix[LINE_MAX_SIZE];
    const char *slash = strrchr(token, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - token);
        if (!dlen) snprintf(dir, sizeof(dir), "/");
        else { memcpy(dir, token, dlen); dir[dlen] = '\0'; }
        snprintf(prefix, sizeof(prefix), "%s", slash + 1);
    } else snprintf(prefix, sizeof(prefix), "%s", token);
    DIR *d = opendir(dir); if (!d) { write(STDOUT_FILENO, "\a", 1); return; }
    char match[LINE_MAX_SIZE] = ""; size_t matches = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (!strncmp(entry->d_name, prefix, strlen(prefix)) &&
            (prefix[0] == '.' || entry->d_name[0] != '.')) {
            snprintf(match, sizeof(match), "%s", entry->d_name); matches++;
        }
    }
    closedir(d);
    if (matches != 1) { write(STDOUT_FILENO, "\a", 1); return; }
    const char *suffix = match + strlen(prefix);
    size_t add = strlen(suffix);
    if (*len + add + 2 >= LINE_MAX_SIZE) return;
    memmove(buf + *cursor + add, buf + *cursor, *len - *cursor + 1);
    memcpy(buf + *cursor, suffix, add); *cursor += add; *len += add;
    char full[LINE_MAX_SIZE];
    snprintf(full, sizeof(full), "%s/%s", dir, match);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        memmove(buf + *cursor + 1, buf + *cursor, *len - *cursor + 1);
        buf[(*cursor)++] = '/'; (*len)++;
    } else if (*cursor == *len) { buf[(*cursor)++] = ' '; buf[++(*len) - 1] = ' '; buf[*len] = '\0'; }
    redraw(buf, *len, *cursor);
}

char *editor_readline(History *h) {
    struct termios old, raw;
    if (tcgetattr(STDIN_FILENO, &old) < 0) return NULL;
    raw = old;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN | ISIG);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return NULL;
    char buf[LINE_MAX_SIZE] = ""; size_t len = 0, cursor = 0;
    size_t history_pos = h->count;
    while (true) {
        unsigned char ch;
        if (read(STDIN_FILENO, &ch, 1) != 1) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); return NULL; }
        if (ch == '\r' || ch == '\n') { write(STDOUT_FILENO, "\r\n", 2); break; }
        if (ch == 4 && len == 0) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); return NULL; }
        if (ch == 3) { len = cursor = 0; buf[0] = '\0'; write(STDOUT_FILENO, "^C\r\n", 4); break; }
        if (ch == '\t') { complete_path(buf, &len, &cursor); continue; }
        if ((ch == 127 || ch == 8) && cursor) {
            memmove(buf + cursor - 1, buf + cursor, len - cursor + 1); cursor--; len--; redraw(buf, len, cursor); continue;
        }
        if (ch == 27) {
            unsigned char seq[2];
            if (read(STDIN_FILENO, seq, 2) != 2 || seq[0] != '[') continue;
            if (seq[1] == 'D' && cursor) { cursor--; write(STDOUT_FILENO, "\033[D", 3); }
            else if (seq[1] == 'C' && cursor < len) { cursor++; write(STDOUT_FILENO, "\033[C", 3); }
            else if (seq[1] == 'A' && history_pos) { history_pos--; replace_line(buf, &len, &cursor, h->items[history_pos]); }
            else if (seq[1] == 'B' && history_pos < h->count) { history_pos++; replace_line(buf, &len, &cursor, history_pos == h->count ? "" : h->items[history_pos]); }
            continue;
        }
        if (isprint(ch) && len + 1 < sizeof(buf)) {
            memmove(buf + cursor + 1, buf + cursor, len - cursor + 1); buf[cursor++] = (char)ch; len++; redraw(buf, len, cursor);
        }
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    return strdup(buf);
}
