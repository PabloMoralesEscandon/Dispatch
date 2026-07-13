#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${DISPATCH_BIN:-$ROOT/dispatch}"
GOLDEN_DIR="$ROOT/tests/golden/tui"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/dispatch-tui-golden.XXXXXX")"
UPDATE_GOLDENS="${UPDATE_GOLDENS:-0}"

trap 'rm -rf "$TMP_ROOT"' EXIT

normalize_frame() {
    sed -E \
        -e "s|$TMP_ROOT|<tmp>|g" \
        -e 's|Repo [^|]*[|]|Repo <path> |' \
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

printf 'PASS: TUI golden frames\n'
