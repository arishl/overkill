#ifndef OVERKILL_EDITOR_H
#define OVERKILL_EDITOR_H

#include <stddef.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
    char path[4096];
} History;

void history_init(History *h);
void history_add(History *h, const char *line);
void history_print(const History *h, size_t limit);
void history_free(History *h);
char *editor_readline(History *h);

#endif
