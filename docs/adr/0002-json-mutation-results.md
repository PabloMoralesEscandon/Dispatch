# 0002: JSON results for mutating commands

- **Status:** Accepted
- **Date:** 2026-11-16

## Context

GitHub issue #3 points out that the commands that change workflow state do not
accept `--json`. After `task add`, `start`, `finish`, or `dep remove`, a tool
has to run a second query to learn the new state, which tasks became ready, or
the ID that `task add` generated, and it has to parse human text on stderr to
tell one failure from another. Agents driving Dispatch hit this on every
lifecycle step.

The issue asks for `--json` on `task add|edit|move|delete`, `group add|ready`,
`dep add|remove`, `ready <id>`, `start`, `finish`, `review`, `unassign`,
`commit add`, and `workspace create|remove|prune`. A success should print the
existing envelope plus a `result` object (action, task, previous and new
state, newly ready tasks). An error should keep its non-zero exit and add a
JSON error with a stable code. Output without `--json` must not change.

The existing JSON contract (version 1) covers read commands only; their
errors are text on stderr, and `ready <id> --json` was rejected outright.

## Decision

**Central dispatch.** The CLI dispatcher keeps a table of the mutating
commands that accept `--json`. It strips the flag after the subcommand, enters
a per-process JSON mode, and, once the command returns, writes the error
document if the exit status is non-zero. Commands report every failure
through one helper, `cli_error(code, task_id, ...)`, which prints the
unchanged text in normal mode and records the code and message in JSON mode.
Every failing path therefore produces exactly one error document, and a path
that forgot to set a code still produces one, with `internal_error`.

**Envelopes.** Task commands reuse the read-command task envelope, with
`tasks` holding the target tasks that still exist after the change. Workspace
commands reuse the `workspace list` envelope, with `workspaces` holding the
records acted on. Removed workspace records are reported as they were just
before removal, with `state: "removed"`, so the consumer still gets the path
and branch. No new envelope shape was invented, so existing parsers apply.

**Result fields.** `result` always contains `action`, `changed`, `task`,
`previous_state`, `new_state`, `targets`, and `newly_ready`, whatever the
command. `targets` lists every task acted on with its own before and after
state, which covers batch commands (`ready A B`, `group ready`). `task`,
`previous_state`, and `new_state` are filled when there is exactly one target
and are `null` otherwise, so the common single-task case needs no array
indexing. Action-specific extras (`review_required`, `previous_assignee`,
`from_group`, `skipped`, ...) are added next to them.

**Newly ready by snapshot diff.** The command records the presentation state
of every task when it first loads the board, and the emitter compares it with
the saved board. `newly_ready` is every task, in board order, that is `ready`
now and was not before. This is computed in one place instead of each command
predicting its side effects, and it is correct for indirect effects such as a
dependent unblocked by `review` or `task delete --force`. It also lists a
target the command readied itself.

**No-ops.** A command that succeeds without changing the board (recording an
already recorded commit, adding an existing dependency, re-linking the same
issue, a `group ready` with nothing to do, a prune that removes nothing)
exits 0 with `changed: false`, matching its text behavior, rather than
becoming an error.

**Errors.** The error document is
`{schema_version, command, error: {code, message, task_id}}` on stderr, and
stdout stays empty. In JSON mode the human error text is not also printed.
When a command reports several error lines (an error and then a usage line, or
a rollback note), the first call sets `code` and `task_id` and later lines are
appended to `message`. The version 1 code list is in the contract.

**Behavior changes.** `ready <id> --json` used to be rejected and is now the
mutating form; `ready --json` without IDs is still the read-only queue. A
repeated `--json` on a mutating command is a `usage` error. Human
confirmation lines (`Started DE-01`, `Created workspace ...`) are not printed
in JSON mode.

**Versioning.** Every change is additive for existing consumers: read-command
output is unchanged and the new fields and documents appear only where
`--json` was previously not accepted. `schema_version` stays 1.

## Consequences

- An agent can run a lifecycle step and get the new state, the generated task
  ID, and the tasks it unblocked from one command, and can branch on
  `error.code` instead of parsing messages.
- Adding `--json` to another mutating command means adding a table entry,
  building a result, and making sure its failures go through `cli_error`.
- The snapshot copies every task ID and state once per JSON mutation. Boards
  are small, so the cost is negligible.
- Non-fatal warnings unrelated to the failure, such as a failed
  `dispatch.log` append, can still appear on stderr as text in JSON mode.
- Shell completion scripts do not offer `--json` for the mutating commands,
  and `--help` does not list it.

### Follow-ups

These were left out of scope by the project owner and remain open:

- JSON error documents for read commands, which still report errors as text.
- `--json` for the other mutating commands: `task priority`, `group edit`,
  `workspace repair`, the `agent` commands, `normalize`, `init`, and
  `repo set`.
