# overkill

`overkill` is a small context-aware project shell written in C. Its prompt discovers
the current project and shows useful state such as the Git branch, dirty status,
language, build system, Python virtual environment, and container configuration.

```text
╭─ ~/Dev/CS6200/project1 ─────────────────────
│ git: main ✓    lang: C    build: make    lima: cs6200    jobs: 0
╰─❯
```

## Build and run

```sh
make
./overkill
```

Commands are run by `/bin/sh`, so pipes, redirects, substitutions, and existing
command-line tools work normally. `cd`, `context`, `trust`, `help`, and `exit` are
built in. Set `NO_COLOR=1` to disable ANSI colors.

## Interactive editing

- Left/right arrows move the cursor; backspace edits in place.
- Up/down arrows navigate persistent history in `~/.overkill_history`.
- `history` or `history --full` prints the complete numbered command history;
  `history 20` prints only the latest 20 entries.
- Tab completes a unique filesystem path and appends `/` for directories.
- Ctrl-C clears the current input and Ctrl-D exits from an empty prompt.
- `help` opens the categorized command menu; `help <command>` shows detailed usage.

## Configuration and project hooks

Global environment configuration lives in `~/.overkillrc`:

```sh
export EDITOR=vim
unset OLD_SETTING
```

A project may have its own `.overkillrc`, using the same environment syntax plus an
optional hook:

```sh
export API_ENV=development
on_enter=printf 'entered this project\n'
```

Project configuration is not loaded until you run `trust` from inside that
project. Trusted absolute paths are stored in `~/.overkill_trusted`. Review the file
before trusting it: `on_enter` is executable shell code.

`overkill` ignores malformed inherited macOS `LSCOLORS` values, preventing them from
breaking every invocation of `ls`. This repair is silent; set `OVERKILL_DEBUG=1` to
diagnose it. Generic workspace containers named `Dev`,
`Projects`, `Code`, `src`, or `repos` are also excluded from language detection,
so a stray manifest in a workspace directory does not leak into its prompt.

The prompt always reports `vm no` on a detected host or `vm yes (type)` for Lima,
WSL, containers, and recognized hypervisors. Set `OVERKILL_VM=qemu` to identify an
otherwise ambiguous guest, or `OVERKILL_VM=host` to force host mode.

Job control and full POSIX-shell grammar are delegated to `/bin/sh`; native
foreground commands therefore retain pipes, redirects, and normal shell syntax.

## Project commands

`overkill` maps manifests to project types and chooses the corresponding action:

| Marker | Project | `build` | `run` |
|---|---|---|---|
| `CMakeLists.txt` | C/C++ | `cmake --build build` | `ctest --test-dir build` |
| `Makefile` | C/C++ | `make` | executable or `make run` |
| `Cargo.toml` | Rust | `cargo build` | `cargo run` |
| `package.json` | Node | `npm run build` | `npm start` |
| `go.mod` | Go | `go build ./...` | `go run .` |
| `pyproject.toml` | Python | `python -m build` | `python .` |

Builds report elapsed wall-clock time. Override inference in a trusted project
`.overkillrc` with `build=...` and `run=...`. Set `lima=cs6200` there, or put the
instance name in a project `.lima` file, to show it in the prompt.

Other project-aware commands:

- `files` recursively summarizes file extensions, excluding generated/vendor trees.
- `todo` reports TODO/FIXME comments with file and line number.
- `changed` prints concise Git changes.
- `ports` shows listening TCP ports and owning processes (`lsof` on macOS, `ss` on Linux).

## Directory state and resume

For interactive sessions, the current project, subdirectory, and last three commands
are stored under `~/.overkill_state`. Returning to a project prints a “Welcome back”
summary plus its Git changes. From elsewhere, `resume` returns to the last saved
project subdirectory. It does not automatically rerun commands.

## Managed processes

```sh
start ./server
jobs
stop 1
restart 1
```

`start` creates a separate process group and redirects output to
`.ctx/logs/<id>.log`. `jobs` shows the ID, PID, command, CPU, memory, age, and
lifecycle state; the prompt shows the running count. `stop` sends `SIGTERM` to the
whole process group, while `restart` relaunches its saved command. Remaining
managed processes receive `SIGTERM` when `overkill` exits.

## Migration from ctxsh

The executable and all new configuration files use the `overkill` name. Existing
`.ctxshrc`, history, trust, state, and `CTXSH_*` environment settings are still
read as a compatibility fallback; newly written data uses the `overkill` paths.
