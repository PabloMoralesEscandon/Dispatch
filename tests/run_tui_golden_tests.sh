#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${DISPATCH_BIN:-$ROOT/dispatch}"
GOLDEN_DIR="${TUI_GOLDEN_DIR:-$ROOT/tests/golden/tui}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/dispatch-tui-golden.XXXXXX")"
# Dispatch stores realpaths (/private/var/... for a macOS TMPDIR), so match
# paths in captured text against the resolved root as well.
TMP_ROOT_REAL="$(cd "$TMP_ROOT" && pwd -P)"
UPDATE_GOLDENS="${UPDATE_GOLDENS:-0}"
MUTATE_FRAME="${TUI_GOLDEN_MUTATE_FRAME:-}"

trap 'rm -rf "$TMP_ROOT"' EXIT

if [ "$UPDATE_GOLDENS" = "1" ] && [ -n "$MUTATE_FRAME" ]; then
    printf 'UPDATE_GOLDENS and TUI_GOLDEN_MUTATE_FRAME are mutually exclusive\n' >&2
    exit 1
fi

# shellcheck source=tests/utf8_locale.sh
. "$ROOT/tests/utf8_locale.sh"

# A path inside a frame line (the board header, inspector fields) is cut to
# the frame width at a point that depends on how long this machine's temp path
# is, and no rewrite of the captured text can undo that.
# DISPATCH_TUI_SMOKE_TMP_ROOT makes the render itself print "<tmp>" before
# layout (docs/adr/0005-portable-tui-goldens.md). The rules below only cover
# paths that end their line, where nothing follows them.
export DISPATCH_TUI_SMOKE_TMP_ROOT="$TMP_ROOT"

normalize_frame() {
    sed -E \
        -e "s|$TMP_ROOT_REAL|<tmp>|g" \
        -e "s|$TMP_ROOT|<tmp>|g" \
        -e 's|(agent/golden/DE-01)  /.*|\1  <workspace>|' \
        -e 's/[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.+-]+Z?/<timestamp>/g'
}

capture_frame() {
    local name="$1"
    local screen="$2"
    local keys="${3:-}"
    local actual="$TMP_ROOT/$name.txt"

    "$BIN" tui --render-smoke "$screen" 100 30 "$keys" |
        normalize_frame >"$actual"

    if [ "$MUTATE_FRAME" = "$name" ]; then
        # Not sed -i: GNU and BSD sed disagree on its argument.
        sed '2s/DISPATCH/DISPATCH-MUTATED/' "$actual" >"$actual.mutated"
        mv "$actual.mutated" "$actual"
    fi

    if [ "$UPDATE_GOLDENS" = "1" ]; then
        mkdir -p "$GOLDEN_DIR"
        cp "$actual" "$GOLDEN_DIR/$name.txt"
    elif ! diff -u "$GOLDEN_DIR/$name.txt" "$actual"; then
        printf 'FAIL: TUI golden frame changed: %s\n' "$name" >&2
        return 1
    fi
}

WORKFLOW="$TMP_ROOT/workflow"
mkdir -p "$WORKFLOW/repo"
git -C "$WORKFLOW/repo" init -q
git -C "$WORKFLOW/repo" config user.name "Golden Tests"
git -C "$WORKFLOW/repo" config user.email "golden@example.com"
printf 'fixture\n' >"$WORKFLOW/repo/fixture.txt"
git -C "$WORKFLOW/repo" add fixture.txt
git -C "$WORKFLOW/repo" commit -qm "Golden fixture"

cd "$WORKFLOW"
"$BIN" init repo >/dev/null
"$BIN" group add Development --prefix DE >/dev/null
"$BIN" group add Quality --prefix QA >/dev/null
"$BIN" task add DE "Render harness" --description "Capture deterministic terminal frames." >/dev/null
"$BIN" task add QA "Golden assertions" --description "Compare every main TUI view." >/dev/null
"$BIN" dep add DE-01 QA-01 >/dev/null
"$BIN" ready DE-01 --actor user >/dev/null
"$BIN" ready QA-01 --actor user >/dev/null
"$BIN" agent create --name codex-golden --runner codex --no-run-script >/dev/null
"$BIN" workspace create DE-01 --actor golden --dir "$TMP_ROOT/task-workspace" >/dev/null

capture_frame board board
capture_frame task-inspector task
capture_frame agents agents
capture_frame workspaces workspaces
capture_frame logs logs
capture_frame task-form task-form
capture_frame agent-form agent-form
capture_frame group-form group-form
capture_frame help board "?"

# FX-19: a separate fixture for non-ASCII content, so the accented frames can
# grow without churning every frame above. An accented row sits next to a
# plain one to make column drift visible in the diff, and the descriptions are
# long enough to wrap.
ACCENTED="$TMP_ROOT/accented"
mkdir -p "$ACCENTED/repo"
git -C "$ACCENTED/repo" init -q
git -C "$ACCENTED/repo" config user.name "Golden Tests"
git -C "$ACCENTED/repo" config user.email "golden@example.com"
printf 'fixture\n' >"$ACCENTED/repo/fixture.txt"
git -C "$ACCENTED/repo" add fixture.txt
git -C "$ACCENTED/repo" commit -qm "Golden fixture"

cd "$ACCENTED"
"$BIN" init repo >/dev/null
"$BIN" group add "Traducción" --prefix TR >/dev/null
"$BIN" task add TR "Revisión de traducción acentuada" \
    --description "Una descripción larga con acentos que debería envolver sobre varias líneas sin partir ningún carácter multibyte por la mitad." >/dev/null
"$BIN" task add TR "Plain ASCII row for alignment" \
    --description "An unaccented row whose columns must line up with the accented one above." >/dev/null
"$BIN" ready TR-01 --actor user >/dev/null

capture_frame accented-board board
capture_frame accented-task board i
# Only the accented bytes are safe to type here: the headless key path routes
# every ASCII letter to its own board shortcut before the search buffer.
capture_frame accented-search board "/ó"

# GH-1: a task with a linked GitHub issue, kept in its own fixture so the
# issue_link log record does not churn the logs frame above.
ISSUE="$TMP_ROOT/issue"
mkdir -p "$ISSUE/repo"
git -C "$ISSUE/repo" init -q

cd "$ISSUE"
"$BIN" init repo >/dev/null
"$BIN" group add Development --prefix DE >/dev/null
"$BIN" task add DE "Trace work to GitHub" \
    --description "Show the linked issue in the task inspector." \
    --issue https://github.com/acme/widgets/issues/12 >/dev/null
"$BIN" ready DE-01 --actor user >/dev/null

capture_frame issue-task task

printf 'PASS: TUI golden frames\n'
