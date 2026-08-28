CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean test install

all: overkill

overkill: src/overkill.c src/editor.c src/editor.h src/process.c src/process.h src/project.c src/project.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/overkill.c src/editor.c src/process.c src/project.c

test: overkill
	./tests/smoke.sh

install: overkill
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 overkill "$(DESTDIR)$(BINDIR)/overkill"
	@echo "Installed overkill to $(DESTDIR)$(BINDIR)/overkill"

clean:
	rm -f overkill ctxsh
