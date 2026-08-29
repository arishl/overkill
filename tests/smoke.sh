#!/bin/sh
set -eu

output=$(printf 'context\nprintf "hello\\n"\nexit\n' | NO_COLOR=1 ./overkill)
printf '%s\n' "$output" | grep 'lang C' >/dev/null
printf '%s\n' "$output" | grep 'build make' >/dev/null
printf '%s\n' "$output" | grep 'vm ' >/dev/null
printf '%s\n' "$output" | grep 'hello' >/dev/null
./overkill -c 'test 2 -eq 2'

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
mkdir "$tmpdir/Dev"
printf '{}\n' > "$tmpdir/Dev/package.json"
workspace_output=$(cd "$tmpdir/Dev" && printf 'context\nexit\n' | NO_COLOR=1 "$OLDPWD/overkill")
if printf '%s\n' "$workspace_output" | grep 'lang: JS/TS' >/dev/null; then
    printf 'workspace container incorrectly detected as a project\n' >&2
    exit 1
fi

color_output=$(LSCOLORS=Gxexport ./overkill -c 'printf %s "${LSCOLORS-unset}"' 2>/dev/null)
test "$color_output" = unset

vm_output=$(printf 'context\nexit\n' | OVERKILL_VM=qemu NO_COLOR=1 ./overkill)
printf '%s\n' "$vm_output" | grep 'vm yes (qemu)' >/dev/null

feature_output=$(printf 'files\ntodo\nchanged\nexit\n' | NO_COLOR=1 ./overkill)
printf '%s\n' "$feature_output" | grep '\.c' >/dev/null
printf '%s\n' "$feature_output" | grep 'TODO' >/dev/null

overkill_bin=$PWD/overkill
mkdir "$tmpdir/managed"
printf 'all:\n\t@true\n' > "$tmpdir/managed/Makefile"
managed_output=$(cd "$tmpdir/managed" && printf 'start sleep 30\njobs\nlogs 1\nstop 1\nexit\n' | NO_COLOR=1 "$overkill_bin")
printf '%s\n' "$managed_output" | grep 'sleep 30' >/dev/null
printf '%s\n' "$managed_output" | grep 'stopped' >/dev/null

mkdir -p "$tmpdir/qol/sub" "$tmpdir/qol-home"
printf 'all:\n\t@true\n' > "$tmpdir/qol/Makefile"
qol_output=$(cd "$tmpdir/qol/sub" && printf 'mark project\nmkcd scratch/deep\nup 2\njump project\nroot\npwd\ndoctor\nexit\n' | HOME="$tmpdir/qol-home" NO_COLOR=1 "$overkill_bin")
printf '%s\n' "$qol_output" | grep 'Marked project' >/dev/null
printf '%s\n' "$qol_output" | grep "$tmpdir/qol$" >/dev/null
printf '%s\n' "$qol_output" | grep 'Overkill doctor' >/dev/null

mkdir "$tmpdir/cmake" "$tmpdir/rust" "$tmpdir/node" "$tmpdir/python" "$tmpdir/go"
: > "$tmpdir/cmake/CMakeLists.txt"
: > "$tmpdir/rust/Cargo.toml"
: > "$tmpdir/node/package.json"
: > "$tmpdir/python/pyproject.toml"
: > "$tmpdir/go/go.mod"
for spec in 'cmake:cmake' 'rust:cargo' 'node:npm' 'python:python' 'go:go'; do
    dir=${spec%%:*}; expected=${spec#*:}
    detected=$(cd "$tmpdir/$dir" && printf 'context\nexit\n' | NO_COLOR=1 "$overkill_bin")
    printf '%s\n' "$detected" | grep "build $expected" >/dev/null
done

mkdir "$tmpdir/home"
printf 'echo first\necho second\necho second\n' > "$tmpdir/home/.overkill_history"
history_output=$(printf 'history --full\nexit\n' | HOME="$tmpdir/home" NO_COLOR=1 ./overkill)
printf '%s\n' "$history_output" | grep '1  echo first' >/dev/null
test "$(printf '%s\n' "$history_output" | grep -c 'echo second')" -eq 2
recent_output=$(printf 'history 1\nexit\n' | HOME="$tmpdir/home" NO_COLOR=1 ./overkill)
printf '%s\n' "$recent_output" | grep '3  echo second' >/dev/null

help_output=$(printf 'help\nhelp build\nexit\n' | HOME="$tmpdir/home" NO_COLOR=1 ./overkill)
printf '%s\n' "$help_output" | grep '^PROJECT$' >/dev/null
printf '%s\n' "$help_output" | grep 'build — Build the detected project' >/dev/null
printf 'overkill smoke tests passed\n'
