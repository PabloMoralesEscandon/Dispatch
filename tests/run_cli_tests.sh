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

assert_line() {
    if ! printf '%s\n' "$RUN_OUTPUT" | grep -Fxq -- "$1"; then
        fail "expected output to contain line: $1"
    fi
}

assert_not_contains() {
    if printf '%s\n' "$RUN_OUTPUT" | grep -Fq -- "$1"; then
        fail "expected output not to contain: $1"
    fi
}

assert_file_contains() {
    local file="$1"
    local text="$2"
    if ! grep -Fq -- "$text" "$file"; then
        fail "expected $file to contain: $text"
    fi
}

assert_file_not_contains() {
    local file="$1"
    local text="$2"
    if grep -Fq -- "$text" "$file"; then
        fail "expected $file not to contain: $text"
    fi
}

line_count() {
    wc -l < "$1" | tr -d ' '
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

cc -Iinclude tests/lock_primitive_test.c src/dispatch_store.c src/dispatch.c \
    -ljansson -o "$TMP_ROOT/lock_primitive_test"
"$TMP_ROOT/lock_primitive_test" "$TMP_ROOT/dispatch.json" >/dev/null

cc -Iinclude tests/store_records_test.c src/dispatch_store.c src/dispatch.c \
    -ljansson -o "$TMP_ROOT/store_records_test"
"$TMP_ROOT/store_records_test" "$TMP_ROOT/empty-records.json" \
    "$TMP_ROOT/populated-records.json" >/dev/null

cc -Iinclude tests/log_writer_test.c src/dispatch_store.c src/dispatch.c \
    -ljansson -o "$TMP_ROOT/log_writer_test"
"$TMP_ROOT/log_writer_test" "$TMP_ROOT/dispatch.log" >/dev/null

cc -Iinclude tests/workspace_naming_test.c src/dispatch.c \
    -o "$TMP_ROOT/workspace_naming_test"
"$TMP_ROOT/workspace_naming_test" "$TMP_ROOT/workspace-naming" >/dev/null

case_dir="$(make_case_dir core)"
cd "$case_dir"
mkdir repo
cat >AGENTS.md <<'AGENTS'
# Dispatch Agent Instructions

Use the Dispatch CLI for all workflow state.
Never read dispatch.json directly.
AGENTS

expect_ok "$BIN" init repo
assert_contains "Created dispatch.json for repo repo"

if [ ! -f dispatch.json ]; then
    fail "dispatch init did not create dispatch.json"
fi

expect_ok "$BIN" init
assert_contains "dispatch.json already exists"

printf 'locked\n' > dispatch.json.lock
expect_fail "$BIN" group add Locked --prefix LK
assert_contains "Dispatch board is locked by another process; retry shortly."
expect_fail "$BIN" normalize
assert_contains "Dispatch board is locked by another process; retry shortly."
rm -f dispatch.json.lock

expect_ok "$BIN" agent list
assert_contains "(no agents)"

expect_fail "$BIN" agent create --runner codex
assert_contains "Agent name must contain only letters, digits, '-' or '_'"

expect_fail "$BIN" agent create --name bad/name --runner codex
assert_contains "Agent name must contain only letters, digits, '-' or '_'"

expect_fail "$BIN" agent create --name codex-a --runner unknown
assert_contains "Agent runner must be codex or claude"

expect_ok "$BIN" agent create --name codex-a --runner codex --model gpt-test --print-command
assert_contains "Created agent codex-a (codex)"
assert_contains "prompt: .dispatch/agents/codex-a/codex-a-PROMPT.md"
assert_contains "run script: .dispatch/agents/codex-a/run.sh"
assert_contains "command: codex --model gpt-test \"\$(cat '.dispatch/agents/codex-a/codex-a-PROMPT.md')\""

expect_ok "$BIN" agent list
assert_contains "codex-a"
assert_contains "codex"
assert_contains ".dispatch/agents/codex-a"

expect_ok "$BIN" agent show codex-a
assert_contains "Name: codex-a"
assert_contains "Runner: codex"
assert_contains "Status: enabled"
assert_contains "Model: gpt-test"
assert_contains "Agent dir: .dispatch/agents/codex-a"
assert_contains "Prompt: .dispatch/agents/codex-a/codex-a-PROMPT.md"
assert_contains "Run script: .dispatch/agents/codex-a/run.sh"
assert_contains "Session ID: -"
assert_contains "Current task: -"
assert_contains "Last workspace: -"

expect_ok "$BIN" agent command codex-a
assert_contains "codex --model gpt-test \"\$(cat '.dispatch/agents/codex-a/codex-a-PROMPT.md')\""

expect_ok "$BIN" agent resume codex-a
assert_contains "codex resume --model 'gpt-test' --last"

expect_fail "$BIN" agent show missing-agent
assert_contains "No agent named missing-agent"

expect_fail "$BIN" agent command missing-agent
assert_contains "No agent named missing-agent"

expect_fail "$BIN" agent resume missing-agent
assert_contains "No agent named missing-agent"

expect_fail "$BIN" agent command codex-a --bad
assert_contains "Unknown agent command option: --bad"

expect_fail "$BIN" agent resume codex-a --bad
assert_contains "Unknown agent resume option: --bad"

if [ ! -f .dispatch/agents/codex-a/codex-a-PROMPT.md ]; then
    fail "agent prompt was not created"
fi
assert_file_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "# Dispatch Agent: codex-a"
assert_file_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "- Agent name: \`codex-a\`"
assert_file_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "## Agent ID"
assert_file_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "## Actor Usage"
assert_file_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "dispatch start <TASK-ID> --actor codex-a"
assert_file_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "dispatch workspace create <TASK-ID> --actor codex-a"
assert_file_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "dispatch agent session codex-a --session-id <SESSION-ID>"
assert_file_not_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "# Dispatch Agent Instructions"
assert_file_not_contains .dispatch/agents/codex-a/codex-a-PROMPT.md "Never read dispatch.json directly."
if [ ! -d .dispatch/agents/codex-a/scratch ]; then
    fail "agent scratch directory was not created"
fi
if [ ! -d .dispatch/agents/codex-a/decisions ]; then
    fail "agent decisions directory was not created"
fi
if [ ! -x .dispatch/agents/codex-a/run.sh ]; then
    fail "agent run script was not created executable"
fi
mkdir -p fake-bin
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$@"\n' > fake-bin/codex
chmod +x fake-bin/codex
PATH="$case_dir/fake-bin:$PATH" expect_ok .dispatch/agents/codex-a/run.sh
assert_contains "# Dispatch Agent: codex-a"

expect_fail "$BIN" agent create --name codex-a --runner codex
assert_contains "Agent codex-a already exists"

expect_ok "$BIN" agent create --name claude-a --runner claude --no-run-script --print-command
assert_contains "Created agent claude-a (claude)"
assert_contains "command: claude --prompt-file \".dispatch/agents/claude-a/claude-a-PROMPT.md\""
expect_ok "$BIN" agent command claude-a --print-command
assert_contains "claude --prompt-file \".dispatch/agents/claude-a/claude-a-PROMPT.md\""
expect_ok "$BIN" agent show claude-a
assert_contains "Run script: -"
expect_ok "$BIN" agent resume claude-a
assert_contains "claude --session-id '"
assert_contains "\"\$(cat '.dispatch/agents/claude-a/claude-a-PROMPT.md')\""
assert_contains "resume script: .dispatch/agents/claude-a/resume.sh"
if [ ! -x .dispatch/agents/claude-a/resume.sh ]; then
    fail "agent resume script was not created executable"
fi
expect_ok "$BIN" agent show claude-a
assert_not_contains "Session ID: -"
expect_ok "$BIN" agent resume claude-a --print-command
assert_contains "claude --resume '"
assert_contains "resume script: .dispatch/agents/claude-a/resume.sh"
if [ -e .dispatch/agents/claude-a/run.sh ]; then
    fail "agent run script was created despite --no-run-script"
fi

expect_ok "$BIN" agent create --name archive-me --runner codex --no-run-script
expect_ok "$BIN" agent archive archive-me
assert_contains "Archived agent archive-me"

expect_ok "$BIN" agent list
assert_not_contains "archive-me"

expect_ok "$BIN" agent list --all
assert_contains "archive-me"
assert_contains "archived"

expect_ok "$BIN" agent show archive-me
assert_contains "Status: archived"

expect_fail "$BIN" agent command archive-me
assert_contains "Agent archive-me is archived; restore it first"

expect_ok "$BIN" agent restore archive-me
assert_contains "Restored agent archive-me"

expect_ok "$BIN" agent list
assert_contains "archive-me"

expect_ok "$BIN" group add Development --prefix DE
assert_contains "Added group Development (DE)"

expect_fail "$BIN" group add Duplicate --prefix DE
assert_contains "Could not add group"

expect_fail "$BIN" task add DE "DE-99 Bad title"
assert_contains "Task titles should not include Dispatch IDs"

expect_ok "$BIN" task add DE First --description "First task"
assert_contains "Added task DE-01"

expect_ok "$BIN" agent session codex-a --session-id session-1 --current-task DE-01
assert_contains "Updated agent session codex-a"

expect_fail "$BIN" agent archive codex-a
assert_contains "Agent codex-a has active task DE-01"

expect_ok "$BIN" agent show codex-a
assert_contains "Session ID: session-1"
assert_contains "Current task: DE-01"

expect_ok "$BIN" agent resume codex-a
assert_contains "codex resume --model 'gpt-test' 'session-1'"

expect_ok "$BIN" agent session codex-a --clear-session --clear-current-task
assert_contains "Updated agent session codex-a"

expect_ok "$BIN" agent show codex-a
assert_contains "Session ID: -"
assert_contains "Current task: -"

expect_fail "$BIN" task add DE ""
assert_contains "Could not add task"

expect_ok "$BIN" task add DE Second
assert_contains "Added task DE-02"

expect_ok "$BIN" show DE-01
assert_contains "Title: First"
assert_contains "Description: First task"
assert_contains "State: proposed"
assert_contains "Blocks: -"
assert_contains "Commits: -"

expect_ok "$BIN" commit list DE-01
assert_contains "(no commits)"

expect_fail "$BIN" commit add DE-01 not-a-sha
assert_contains "Commit reference must be a 4-64 character hex SHA"

expect_ok "$BIN" commit add DE-01 abc1234 --actor codex
assert_contains "Added commit abc1234 to DE-01"

expect_ok "$BIN" commit add DE-01 abc1234 --actor codex
assert_contains "Commit abc1234 already recorded for DE-01"

expect_ok "$BIN" commit list DE-01
assert_line "abc1234"

expect_ok "$BIN" commit show DE-01
assert_contains "Task: DE-01 First"
assert_contains "  abc1234"

expect_ok "$BIN" show DE-01
assert_contains "Commits: abc1234"

expect_ok "$BIN" dep add DE-01 DE-02
assert_contains "Added dependency DE-01 -> DE-02 (DE-02 depends on DE-01)"

expect_ok "$BIN" list
assert_contains "[DE] Development"
assert_contains "  DE-01    proposed   First  commits:1"
assert_contains "  DE-02    proposed   Second  depends_on:DE-01"

expect_ok "$BIN" list
assert_contains "  DE-02    proposed   Second  depends_on:DE-01"

expect_ok "$BIN" blocked
assert_not_contains "DE-02"

expect_fail "$BIN" dep add DE-02 DE-01
assert_contains "Could not add dependency DE-02 -> DE-01 (DE-01 depends on DE-02)"

expect_fail "$BIN" dep add DE-01
assert_contains "Usage: dispatch dep add <dependency-id> <dependent-id>"
assert_contains "Example: dispatch dep add DE-01 DE-02 means DE-02 depends on DE-01"

expect_ok "$BIN" show DE-01
assert_contains "Blocks: DE-02"

expect_ok "$BIN" ready DE-01
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

expect_ok "$BIN" review DE-01
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

case_dir="$(make_case_dir dispatch-log)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
if [ ! -f dispatch.log ]; then
    fail "dispatch init did not create dispatch.log"
fi
assert_file_contains dispatch.log '"command":"init"'
assert_file_contains dispatch.log '"outcome":"success"'

expect_ok "$BIN" group add Development --prefix DE
assert_file_contains dispatch.log '"actor":"user"'
assert_file_contains dispatch.log '"command":"group"'
assert_file_contains dispatch.log '"action":"add"'
assert_file_contains dispatch.log '"group":"DE"'

before_failed="$(line_count dispatch.log)"
expect_fail "$BIN" group add Duplicate --prefix DE
after_failed="$(line_count dispatch.log)"
if [ "$before_failed" -ne "$after_failed" ]; then
    fail "failed mutation appended to dispatch.log"
fi

expect_ok "$BIN" task add DE Root --no-review
expect_ok "$BIN" ready DE-01 --actor alice --no-review
assert_file_contains dispatch.log '"actor":"alice"'
assert_file_contains dispatch.log '"command":"ready"'
assert_file_contains dispatch.log '"task":"DE-01"'
assert_file_contains dispatch.log '"no_review":"true"'

expect_ok "$BIN" start DE-01 --actor codex
expect_ok "$BIN" finish DE-01 --actor codex
assert_file_contains dispatch.log '"actor":"codex"'
assert_file_contains dispatch.log '"command":"finish"'
assert_file_contains dispatch.log '"new_state":"done"'

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

expect_ok "$BIN" group ready DE
assert_contains "Readied 3 tasks in group DE"

expect_ok "$BIN" list
assert_contains "  DE-01    ready      Root"
assert_contains "  DE-02    blocked    AlreadyBlocked  depends_on:DE-01"
assert_contains "  DE-03    blocked    ProposedBlocked  depends_on:DE-01"
assert_contains "  DE-04    ready      Simple"
assert_contains "  DE-05    doing      Active  assigned:codex"
assert_contains "  DE-06    review     Review"
assert_not_contains "  DE-07    done       Done"
assert_contains "  QA-01    proposed   Other"

expect_ok "$BIN" list all
assert_contains "  DE-07    done       Done"
assert_contains "  QA-01    proposed   Other"

expect_ok "$BIN" list all DE
assert_contains "  DE-07    done       Done"
assert_not_contains "  QA-01    proposed   Other"

expect_ok "$BIN" group ready DE --actor user --no-review
assert_contains "Readied 0 tasks in group DE"
expect_ok "$BIN" show DE-04
assert_contains "Requires review: no"
expect_ok "$BIN" show DE-05
assert_contains "Requires review: yes"
expect_ok "$BIN" show DE-06
assert_contains "Requires review: yes"

expect_fail "$BIN" group ready Missing --actor user
assert_contains "No group with id, prefix, or name Missing"

expect_ok "$BIN" group add Docs --prefix DOC
expect_ok "$BIN" task add DOC Notes
expect_ok "$BIN" task add DOC Followup
expect_ok "$BIN" group ready DOC --actor user --no-review
assert_contains "Readied 2 tasks in group DOC"
expect_ok "$BIN" show DOC-01
assert_contains "Requires review: no"
expect_ok "$BIN" show DOC-02
assert_contains "Requires review: no"

case_dir="$(make_case_dir workspace-reserve)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" task add DE Root
expect_ok "$BIN" ready DE-01 --actor user
expect_ok "$BIN" agent create --name Codex_A --runner codex --no-run-script

expect_fail "$BIN" workspace create DE-01
assert_contains "Usage: dispatch workspace create <task-id> --actor <name>"

expect_fail "$BIN" workspace create DE-01 --actor bad/name
assert_contains "Actor must start with an ASCII letter or digit"

expect_fail "$BIN" workspace create Missing --actor codex
assert_contains "No task with id Missing"

expect_fail "$BIN" workspace create DE-01 --actor codex
assert_contains "Configured repository is not a git repository: repo"

expect_ok git -C repo init
expect_ok git -C repo -c user.name=Dispatch -c user.email=dispatch@example.invalid commit --allow-empty -m init
expect_ok "$BIN" workspace create DE-01 --actor Codex_A
assert_contains "Created workspace DE-01 for Codex_A"
assert_contains "branch: agent/codex_a/DE-01"
assert_contains "state: active"
if [ ! -e repo-agent-codex_a-DE-01/.git ]; then
    fail "workspace worktree was not created"
fi

expect_ok "$BIN" agent session Codex_A --last-workspace DE-01
assert_contains "Updated agent session Codex_A"
expect_ok "$BIN" agent show Codex_A
assert_contains "Last workspace: DE-01"
expect_ok "$BIN" agent resume Codex_A
assert_contains "codex resume --cd '"
assert_contains "repo-agent-codex_a-DE-01' --last"
expect_ok "$BIN" agent create --name Claude_A --runner claude --no-run-script
expect_ok "$BIN" agent session Claude_A --session-id 123e4567-e89b-42d3-a456-426614174000 --last-workspace DE-01
expect_ok "$BIN" agent resume Claude_A
assert_contains "cd '"
assert_contains "repo-agent-codex_a-DE-01' && claude --resume '123e4567-e89b-42d3-a456-426614174000'"
expect_ok "$BIN" workspace list
assert_contains "DE-01"
assert_contains "active"
assert_contains "Codex_A"
assert_contains "agent/codex_a/DE-01"

expect_ok "$BIN" workspace show DE-01
assert_contains "Task: DE-01"
assert_contains "Task state: ready"
assert_contains "Workspace state: active"
assert_contains "Actor: Codex_A"
assert_contains "Branch: agent/codex_a/DE-01"
assert_contains "Git worktree: present"

expect_ok "$BIN" show DE-01
assert_contains "Workspace actor: Codex_A"
assert_contains "Workspace path:"
assert_contains "Workspace branch: agent/codex_a/DE-01"

expect_ok "$BIN" list DE
assert_contains "workspace:"
assert_contains "branch: agent/codex_a/DE-01"

expect_fail "$BIN" workspace create DE-01 --actor Codex_A
assert_contains "Workspace already exists for DE-01"

expect_fail "$BIN" start DE-01 --actor other
assert_contains "Workspace for DE-01 belongs to Codex_A"

expect_ok "$BIN" start DE-01 --actor Codex_A
assert_contains "Started DE-01"

expect_ok "$BIN" task add DE Second
expect_fail "$BIN" workspace create DE-02 --actor codex
assert_contains "Task DE-02 must be ready and unassigned"

expect_ok "$BIN" ready DE-02 --actor user
expect_fail "$BIN" workspace create DE-02 --actor codex --branch agent/codex_a/DE-01
assert_contains "Workspace branch already reserved: agent/codex_a/DE-01"

expect_fail "$BIN" workspace create DE-02 --actor codex --dir repo
assert_contains "Workspace path must not equal repository path"

mkdir occupied-workspace
expect_fail "$BIN" workspace create DE-02 --actor codex --dir occupied-workspace
assert_contains "Workspace path already exists:"

expect_ok "$BIN" workspace create DE-02 --actor codex --dir custom-workspace --branch agent/codex/DE-02
assert_contains "Created workspace DE-02 for codex"
assert_contains "branch: agent/codex/DE-02"
assert_contains "state: active"
if [ ! -e custom-workspace/.git ]; then
    fail "custom workspace worktree was not created"
fi

expect_ok "$BIN" task add DE Third
expect_ok "$BIN" ready DE-03 --actor user
expect_fail "$BIN" workspace create DE-03 --actor other --dir custom-workspace
assert_contains "Workspace path already reserved:"

expect_fail "$BIN" workspace remove DE-01
assert_contains "Workspace task DE-01 is doing"

expect_ok "$BIN" workspace remove DE-02
assert_contains "Removed workspace DE-02"
if [ -e custom-workspace ]; then
    fail "custom workspace worktree was not removed"
fi
expect_fail "$BIN" workspace show DE-02
assert_contains "No workspace for DE-02"

case_dir="$(make_case_dir workspace-remove)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok git -C repo init
expect_ok git -C repo -c user.name=Dispatch -c user.email=dispatch@example.invalid commit --allow-empty -m init
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" task add DE Dirty
expect_ok "$BIN" ready DE-01 --actor user
expect_ok "$BIN" workspace create DE-01 --actor codex
printf 'dirty\n' >repo-agent-codex-DE-01/dirty.txt

expect_fail "$BIN" workspace remove DE-01
assert_contains "Workspace has uncommitted changes"

expect_ok "$BIN" workspace remove DE-01 --force
assert_contains "Removed workspace DE-01"
if [ -e repo-agent-codex-DE-01 ]; then
    fail "dirty workspace worktree was not force removed"
fi

expect_ok "$BIN" task add DE Active
expect_ok "$BIN" ready DE-02 --actor user
expect_ok "$BIN" workspace create DE-02 --actor codex
expect_ok "$BIN" start DE-02 --actor codex
expect_fail "$BIN" workspace remove DE-02
assert_contains "Workspace task DE-02 is doing"
expect_ok "$BIN" workspace remove DE-02 --force
assert_contains "Removed workspace DE-02"
if [ -e repo-agent-codex-DE-02 ]; then
    fail "active workspace worktree was not force removed"
fi

case_dir="$(make_case_dir workspace-prune)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok git -C repo init
expect_ok git -C repo -c user.name=Dispatch -c user.email=dispatch@example.invalid commit --allow-empty -m init
expect_ok "$BIN" group add Development --prefix DE

expect_fail "$BIN" workspace prune
assert_contains "Usage: dispatch workspace prune"

expect_ok "$BIN" task add DE Clean
expect_ok "$BIN" ready DE-01 --actor user
expect_ok "$BIN" workspace create DE-01 --actor codex
expect_ok "$BIN" start DE-01 --actor codex
expect_ok "$BIN" finish DE-01 --actor codex
expect_ok "$BIN" review DE-01 --actor user

expect_ok "$BIN" task add DE Dirty
expect_ok "$BIN" ready DE-02 --actor user
expect_ok "$BIN" workspace create DE-02 --actor codex
expect_ok "$BIN" start DE-02 --actor codex
expect_ok "$BIN" finish DE-02 --actor codex
expect_ok "$BIN" review DE-02 --actor user
printf 'dirty\n' >repo-agent-codex-DE-02/dirty.txt

expect_ok "$BIN" task add DE Active
expect_ok "$BIN" ready DE-03 --actor user
expect_ok "$BIN" workspace create DE-03 --actor codex

expect_ok "$BIN" workspace prune --done --dry-run
assert_contains "Would remove done workspace DE-01"
assert_contains "Skipped done workspace DE-02: workspace has uncommitted changes"
if [ ! -e repo-agent-codex-DE-01/.git ]; then
    fail "dry-run removed clean done workspace"
fi
expect_ok "$BIN" workspace show DE-01

expect_ok "$BIN" workspace prune --done
assert_contains "Removed done workspace DE-01"
assert_contains "Skipped done workspace DE-02: workspace has uncommitted changes"
if [ -e repo-agent-codex-DE-01 ]; then
    fail "clean done workspace worktree was not pruned"
fi
expect_fail "$BIN" workspace show DE-01
assert_contains "No workspace for DE-01"
expect_ok "$BIN" workspace show DE-02
expect_ok "$BIN" workspace show DE-03

case_dir="$(make_case_dir workspace-inspect)"
cd "$case_dir"
cp "$ROOT/tests/fixtures/workspace-records-board.json" dispatch.json

expect_ok "$BIN" workspace list
assert_contains "DE-01"
assert_contains "active"
assert_contains "codex"
assert_contains "agent/codex/DE-01"
assert_contains "DE-02"
assert_contains "creating"

expect_ok "$BIN" workspace show DE-01
assert_contains "Task: DE-01"
assert_contains "Workspace state: active"
assert_contains "Git worktree: missing"

expect_ok "$BIN" workspace show DE-02
assert_contains "Task: DE-02"
assert_contains "Workspace state: creating"
assert_contains "Git worktree: missing"

expect_fail "$BIN" workspace show Missing
assert_contains "No workspace for Missing"

expect_fail "$BIN" workspace remove DE-01 --force
assert_contains "Workspace path is not a git worktree"

expect_ok "$BIN" workspace prune --stale --dry-run
assert_contains "Would prune stale workspace DE-02"
expect_ok "$BIN" workspace show DE-02

expect_ok "$BIN" workspace prune --stale
assert_contains "Pruned stale workspace DE-02"
expect_fail "$BIN" workspace show DE-02
assert_contains "No workspace for DE-02"
expect_ok "$BIN" workspace show DE-01

case_dir="$(make_case_dir completion-candidates)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok git -C repo init
expect_ok git -C repo -c user.name=Dispatch -c user.email=dispatch@example.invalid commit --allow-empty -m init
expect_ok "$BIN" agent create --name codex-a --runner codex
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" group add QA --prefix QA
expect_ok "$BIN" task add DE Root
expect_ok "$BIN" task add QA Check
expect_ok "$BIN" ready DE-01 --actor user
expect_ok "$BIN" workspace create DE-01 --actor codex-a

expect_fail "$BIN" agent archive codex-a
assert_contains "Agent codex-a has active workspace"

expect_ok "$BIN" completion candidates commands
assert_line "completion"
assert_line "commit"
assert_line "workspace"

expect_ok "$BIN" completion candidates candidate-kinds
assert_line "tasks"
assert_line "groups"
assert_line "agents"
assert_line "workspaces"

expect_ok "$BIN" completion candidates tasks
assert_line "DE-01"
assert_line "QA-01"

expect_ok "$BIN" completion candidates groups
assert_line "DE"
assert_line "QA"

expect_ok "$BIN" completion candidates agents
assert_line "codex-a"

expect_ok "$BIN" completion candidates workspaces
assert_line "DE-01"

expect_fail "$BIN" completion candidates unknown
assert_contains "Usage: dispatch completion candidates"

expect_ok "$BIN" completion zsh
assert_contains "#compdef dispatch"
assert_contains "subcommands=(candidates bash fish zsh)"
assert_contains 'dispatch completion candidates "$1"'
assert_contains "compadd -- all"
assert_contains "_dispatch_compadd_candidates tasks"

expect_fail "$BIN" completion zsh extra
assert_contains "Usage: dispatch completion candidates"

expect_ok "$BIN" completion bash
assert_contains "complete -F _dispatch_complete dispatch"
assert_contains 'dispatch completion candidates "$1"'
assert_contains "candidates bash fish zsh"
assert_contains "all \$(_dispatch_candidate_values groups)"
assert_contains "_dispatch_complete_candidates tasks"
assert_contains "--actor --no-review"

expect_fail "$BIN" completion bash extra
assert_contains "Usage: dispatch completion candidates"

expect_ok "$BIN" completion fish
assert_contains "function __dispatch_command"
assert_contains "function __dispatch_candidates"
assert_contains "command \$cmd completion candidates \$argv[1]"
assert_contains "candidates bash fish zsh"
assert_contains "all (__dispatch_candidates groups)"
assert_contains "(__dispatch_candidates tasks)"
assert_contains "(__dispatch_candidates workspaces)"

if command -v fish >/dev/null 2>&1; then
    "$BIN" completion fish > "$TMP_ROOT/dispatch-completion.fish"
    expect_ok fish -c "cd '$ROOT'; source '$TMP_ROOT/dispatch-completion.fish'; complete -C './dispatch '"
    assert_line "show"
fi

expect_fail "$BIN" completion fish extra
assert_contains "Usage: dispatch completion candidates"

case_dir="$(make_case_dir workspace-sequence)"
cd "$case_dir"
mkdir repo

expect_ok "$BIN" init repo
expect_ok git -C repo init
expect_ok git -C repo -c user.name=Dispatch -c user.email=dispatch@example.invalid commit --allow-empty -m init
expect_ok "$BIN" group add Development --prefix DE
expect_ok "$BIN" task add DE First --no-review
expect_ok "$BIN" task add DE Second --no-review
expect_ok "$BIN" task add DE Gate
expect_ok "$BIN" dep add DE-01 DE-02
expect_ok "$BIN" dep add DE-02 DE-03
expect_ok "$BIN" ready DE-01 --actor user
expect_ok "$BIN" ready DE-02 --actor user
expect_ok "$BIN" ready DE-03 --actor user

expect_ok "$BIN" workspace create DE-01 --actor seq --sequence
assert_contains "branch: agent/seq/DE-01-sequence"
assert_contains "tasks: DE-01,DE-02,DE-03"
assert_contains "review gate: DE-03"

expect_ok "$BIN" workspace show DE-02
assert_contains "Task: DE-01"
assert_contains "Sequence tasks: DE-01,DE-02,DE-03"
assert_contains "Review gate: DE-03"

expect_fail "$BIN" start DE-02 --actor other
assert_contains "Workspace for DE-02 belongs to seq"

expect_ok "$BIN" start DE-01 --actor seq
expect_ok "$BIN" finish DE-01 --actor seq
assert_contains "Finished DE-01 (done)"
expect_ok "$BIN" start DE-02 --actor seq

expect_ok "$BIN" task add DE BranchRoot --no-review
expect_ok "$BIN" task add DE BranchA
expect_ok "$BIN" task add DE BranchB
expect_ok "$BIN" dep add DE-04 DE-05
expect_ok "$BIN" dep add DE-04 DE-06
expect_ok "$BIN" ready DE-04 --actor user
expect_fail "$BIN" workspace create DE-04 --actor seq --sequence
assert_contains "Sequence task DE-04 must have exactly one dependent"

expect_ok "$BIN" task add DE JoinRoot --no-review
expect_ok "$BIN" task add DE Other
expect_ok "$BIN" task add DE JoinGate
expect_ok "$BIN" dep add DE-07 DE-09
expect_ok "$BIN" dep add DE-08 DE-09
expect_ok "$BIN" ready DE-07 --actor user
expect_fail "$BIN" workspace create DE-07 --actor seq --sequence
assert_contains "Sequence task DE-09 must depend only on DE-07"

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

expect_ok "$BIN" list DE
assert_contains "[DE] Development"
assert_contains "  (done)"
assert_not_contains "  DE-01    done       DirectFinish"

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
assert_contains "agent create/list/show/command/session/resume"
assert_contains "workspace create/list/show/remove/prune"
assert_contains "completion candidates"
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
assert_contains "Usage: dispatch list [all] [group]"

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
assert_contains "  DE-02    proposed   BranchA  depends_on:DE-01"
assert_contains "  DE-03    proposed   BranchB  depends_on:DE-01"
assert_contains "  DE-04    proposed   Join  depends_on:DE-02,DE-03"

expect_ok env -u NO_COLOR FORCE_COLOR=1 "$BIN" list DE
assert_contains "${ESC}[1;36m[DE] Development${ESC}[0m"
assert_contains "${ESC}[2;37mproposed  ${ESC}[0m Join"
assert_contains "${ESC}[2;37mdepends_on:DE-02,DE-03${ESC}[0m"

expect_ok env FORCE_COLOR=1 NO_COLOR=1 "$BIN" list DE
assert_not_contains "${ESC}["

printf 'PASS: Dispatch CLI tests\n'
