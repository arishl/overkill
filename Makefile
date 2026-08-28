CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L

.PHONY: all clean test

all: overkill

overkill: src/overkill.c src/editor.c src/editor.h src/process.c src/process.h src/project.c src/project.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/overkill.c src/editor.c src/process.c src/project.c

test: overkill
	./tests/smoke.sh

clean:
	rm -f overkill ctxsh
