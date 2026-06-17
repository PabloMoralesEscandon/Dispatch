#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/dispatch"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/dispatch-cli-tests.XXXXXX")"
RUN_OUTPUT=""
RUN_STATUS=0
ESC=$'\033'

trap 'rm -rf "$TMP_ROOT"' EXIT

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    if [ -n "$RUN_OUTPUT" ]; then
        printf '%s\n' "$RUN_OUTPUT" >&2
    fi
    exit 1
}

run_cmd() {
    set +e
    RUN_OUTPUT="$("$@" 2>&1)"
    RUN_STATUS=$?
    set -e
}

expect_ok() {
    run_cmd "$@"
    if [ "$RUN_STATUS" -ne 0 ]; then
        fail "expected success: $*"
    fi
}

expect_fail() {
    run_cmd "$@"
    if [ "$RUN_STATUS" -eq 0 ]; then
        fail "expected failure: $*"
    fi
}

assert_contains() {
    if ! printf '%s\n' "$RUN_OUTPUT" | grep -Fq -- "$1"; then
        fail "expected output to contain: $1"
    fi
}

assert_not_contains() {
    if printf '%s\n' "$RUN_OUTPUT" | grep -Fq -- "$1"; then
        fail "expected output not to contain: $1"
    fi
}

make_case_dir() {
    local name="$1"
    local dir="$TMP_ROOT/$name"
    mkdir -p "$dir"
    printf '%s\n' "$dir"
}

cd "$ROOT"
if [ ! -x ./nob ]; then
    cc nob.c -o nob
fi
./nob >/dev/null

case_dir="$(make_case_dir core)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
assert_contains "Created dispatch.json for repo repo"

if [ ! -f dispatch.json ]; then
    fail "dispatch init did not create dispatch.json"
fi

expect_ok "$BIN" init
assert_contains "dispatch.json already exists"

expect_ok "$BIN" group add Development --prefix DE
assert_contains "Added group Development (DE)"

expect_fail "$BIN" group add Duplicate --prefix DE
assert_contains "Could not add group"

expect_fail "$BIN" task add DE "DE-99 Bad title"
assert_contains "Task titles should not include Dispatch IDs"

expect_ok "$BIN" task add DE First --description "First task"
assert_contains "Added task DE-01"

expect_fail "$BIN" task add DE ""
assert_contains "Could not add task"

expect_ok "$BIN" task add DE Second
assert_contains "Added task DE-02"

expect_ok "$BIN" show DE-01
assert_contains "Title: First"
assert_contains "Description: First task"
assert_contains "State: proposed"
assert_contains "Blocks: -"

expect_ok "$BIN" dep add DE-01 DE-02
assert_contains "Added dependency DE-01 -> DE-02 (DE-02 depends on DE-01)"

expect_ok "$BIN" list
assert_contains "[DE] Development"
assert_contains "  DE-01    proposed   First"
assert_contains "  DE-02    blocked    Second  depends_on:DE-01"

expect_ok "$BIN" list
assert_contains "  DE-02    blocked    Second  depends_on:DE-01"

expect_fail "$BIN" dep add DE-02 DE-01
assert_contains "Could not add dependency DE-02 -> DE-01 (DE-01 depends on DE-02)"

expect_fail "$BIN" dep add DE-01
assert_contains "Usage: dispatch dep add <dependency-id> <dependent-id>"
assert_contains "Example: dispatch dep add DE-01 DE-02 means DE-02 depends on DE-01"

expect_fail "$BIN" ready DE-01
assert_contains "Usage: dispatch ready <id> --actor <name> [--no-review]"

expect_ok "$BIN" show DE-01
assert_contains "Blocks: DE-02"

expect_ok "$BIN" ready DE-01 --actor user
assert_contains "Readied DE-01"

expect_ok "$BIN" ready DE-02 --actor user
assert_contains "Readied DE-02"

expect_ok "$BIN" blocked
assert_contains "DE-02"
assert_contains "blocked_by: DE-01"

expect_fail "$BIN" start DE-02 --actor codex
assert_contains "Could not start DE-02"

expect_ok "$BIN" start DE-01 --actor codex
assert_contains "Started DE-01"

expect_fail "$BIN" start DE-01 --actor other-agent
assert_contains "Could not start DE-01"

expect_ok "$BIN" show DE-01
assert_contains "State: doing"
assert_contains "Assigned to: codex"
assert_contains "Started by: codex"

expect_ok "$BIN" finish DE-01 --actor codex
assert_contains "Finished DE-01 (review)"
assert_contains "Review required before continuing this sequence."

expect_ok "$BIN" blocked
assert_contains "DE-02"
assert_contains "blocked_by: DE-01"

expect_ok "$BIN" review DE-01 --actor user
assert_contains "Reviewed DE-01"

expect_ok "$BIN" ready
assert_contains "DE-02"
assert_contains "ready"

expect_ok "$BIN" start DE-02 --actor codex
assert_contains "Started DE-02"

expect_ok "$BIN" finish DE-02 --actor codex
assert_contains "Finished DE-02 (review)"

expect_ok "$BIN" review DE-02 --actor user
assert_contains "Reviewed DE-02"

expect_ok "$BIN" task add DE DeleteMe
assert_contains "Added task DE-03"

expect_ok "$BIN" task delete DE-03
assert_contains "Deleted task DE-03"

expect_fail "$BIN" task delete DE-01
assert_contains "Could not delete DE-01"

expect_ok "$BIN" task delete DE-01 --force
assert_contains "Deleted task DE-01"

case_dir="$(make_case_dir id-prefix-display)"
cd "$case_dir"
cp "$ROOT/tests/fixtures/id-prefixed-title-board.json" dispatch.json

expect_ok "$BIN" list FX
assert_contains "  FX-01    proposed   First fixture task"
assert_contains "  FX-02    blocked    Blocked fixture task  depends_on:FX-01"
assert_not_contains "FX-01    proposed   FX-01"
assert_not_contains "FX-02    blocked    FX-02"

expect_ok "$BIN" show FX-01
assert_contains "Title: First fixture task"

expect_ok "$BIN" blocked
assert_contains "FX-02    Blocked fixture task  blocked_by: FX-01"

case_dir="$(make_case_dir group-ready)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" group add QA --prefix QA
expect_ok "$BIN" task add DE Root --no-review
expect_ok "$BIN" task add DE AlreadyBlocked --no-review
expect_ok "$BIN" task add DE ProposedBlocked --no-review
expect_ok "$BIN" task add DE Simple
expect_ok "$BIN" task add DE Active
expect_ok "$BIN" task add DE Review
expect_ok "$BIN" task add DE Done --no-review
expect_ok "$BIN" task add QA Other
expect_ok "$BIN" dep add DE-01 DE-02
expect_ok "$BIN" dep add DE-01 DE-03
expect_ok "$BIN" ready DE-02 --actor user
expect_ok "$BIN" ready DE-05 --actor user
expect_ok "$BIN" start DE-05 --actor codex
expect_ok "$BIN" ready DE-06 --actor user
expect_ok "$BIN" start DE-06 --actor codex
expect_ok "$BIN" finish DE-06 --actor codex
expect_ok "$BIN" ready DE-07 --actor user
expect_ok "$BIN" start DE-07 --actor codex
expect_ok "$BIN" finish DE-07 --actor codex

expect_fail "$BIN" group ready DE
assert_contains "Usage: dispatch group ready <group> --actor <name>"

expect_ok "$BIN" group ready DE --actor user
assert_contains "Readied 3 tasks in group DE"

expect_ok "$BIN" list
assert_contains "  DE-01    ready      Root"
assert_contains "  DE-02    blocked    AlreadyBlocked  depends_on:DE-01"
assert_contains "  DE-03    blocked    ProposedBlocked  depends_on:DE-01"
assert_contains "  DE-04    ready      Simple"
assert_contains "  DE-05    doing      Active  assigned:codex"
assert_contains "  DE-06    review     Review"
assert_contains "  DE-07    done       Done"
assert_contains "  QA-01    proposed   Other"

expect_fail "$BIN" group ready Missing --actor user
assert_contains "No group with id, prefix, or name Missing"

case_dir="$(make_case_dir ready-no-review)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" task add DE DirectFinish
assert_contains "Added task DE-01"

expect_ok "$BIN" ready DE-01 --actor user --no-review
assert_contains "Readied DE-01"

expect_ok "$BIN" show DE-01
assert_contains "Requires review: no"

expect_ok "$BIN" start DE-01 --actor codex
assert_contains "Started DE-01"

expect_ok "$BIN" finish DE-01 --actor codex
assert_contains "Finished DE-01 (done)"

case_dir="$(make_case_dir ungated)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" task add DE Ungated --no-review
assert_contains "Added task DE-01"
expect_ok "$BIN" task add DE AfterUngated
assert_contains "Added task DE-02"
expect_ok "$BIN" dep add DE-01 DE-02
expect_ok "$BIN" ready DE-01 --actor user
expect_ok "$BIN" ready DE-02 --actor user
expect_ok "$BIN" start DE-01 --actor codex
expect_ok "$BIN" finish DE-01 --actor codex
assert_contains "Finished DE-01 (done)"
assert_contains "Next ready tasks:"
assert_contains "DE-02"

case_dir="$(make_case_dir legacy)"
cd "$case_dir"

expect_ok "$BIN" --help
assert_contains "Dispatch: a command line workflow board."
assert_not_contains "--json"
assert_not_contains "clear"
assert_not_contains "project"
assert_not_contains "recurrence"
assert_not_contains "notification"
assert_not_contains "category"
assert_not_contains "  tree"

expect_fail "$BIN" add LegacyTask
assert_contains "Unknown Dispatch command: add"

expect_fail "$BIN" clear
assert_contains "Unknown Dispatch command: clear"

expect_fail "$BIN" pause DE-01 --actor codex
assert_contains "Unknown Dispatch command: pause"

expect_fail "$BIN" tree
assert_contains "Unknown Dispatch command: tree"

expect_fail "$BIN" list projects extra
assert_contains "Usage: dispatch list [group]"

mkdir repo
expect_ok "$BIN" init repo
expect_ok "$BIN" group add Development --prefix DE
expect_fail "$BIN" task add DE Test -d 05-06-2026
assert_contains "Unknown task option: -d"

expect_fail "$BIN" task add DE Test --project old
assert_contains "Unknown task option: --project"

expect_fail "$BIN" task add DE Test --category old
assert_contains "Unknown task option: --category"

expect_fail "$BIN" list Missing
assert_contains "No group with id, prefix, or name Missing"

case_dir="$(make_case_dir list-filter)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" task add DE Root --no-review
expect_ok "$BIN" task add DE BranchA --no-review
expect_ok "$BIN" task add DE BranchB --no-review
expect_ok "$BIN" task add DE Join --no-review
expect_ok "$BIN" dep add DE-01 DE-02
expect_ok "$BIN" dep add DE-01 DE-03
expect_ok "$BIN" dep add DE-02 DE-04
expect_ok "$BIN" dep add DE-03 DE-04
expect_ok "$BIN" list DE
assert_contains "  DE-01    proposed   Root"
assert_contains "  DE-02    blocked    BranchA  depends_on:DE-01"
assert_contains "  DE-03    blocked    BranchB  depends_on:DE-01"
assert_contains "  DE-04    blocked    Join  depends_on:DE-02,DE-03"

expect_ok env -u NO_COLOR FORCE_COLOR=1 "$BIN" list DE
assert_contains "${ESC}[1;36m[DE] Development${ESC}[0m"
assert_contains "${ESC}[1;33mblocked   ${ESC}[0m Join"
assert_contains "${ESC}[2;37mdepends_on:DE-02,DE-03${ESC}[0m"

expect_ok env FORCE_COLOR=1 NO_COLOR=1 "$BIN" list DE
assert_not_contains "${ESC}["

printf 'PASS: Dispatch CLI tests\n'
