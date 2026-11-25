# 0009: JSON errors on reads, non-fatal warnings, and remaining mutations

- **Status:** Accepted
- **Date:** 2026-11-25

## Context

ADR 0002 added `--json` result and error documents to the core task and
workspace mutations, and ADR 0004 added `--json` reads for agents, workspaces,
and `dispatch.log`. Four gaps remained and were tracked in GitHub issue #12:

1. Read commands run with `--json` (`list`, `show`, `ready`, `blocked`,
   `reviews`, `proposed`, `status`, `doctor`, `agent list|show`,
   `workspace list|show`, `log`) still printed errors as plain text on stderr
   instead of the coded JSON error document.
2. The remaining mutating commands (`task priority`, `group edit`,
   `workspace repair`, `agent create|archive|restore|session|command|resume`,
   `normalize`, `init`, and `repo set`) still rejected `--json`.
3. `--help` and the bash, zsh, and fish completion scripts did not list
   `--json` for mutating commands.
4. Non-fatal warnings that occur after a successful save (for example a failed
   `dispatch.log` append, scaffolding failure during `init`, or a non-git
   directory on `repo set`) still printed stray text on stderr or stdout in
   JSON mode.

## Decision

**One central `--json` dispatch table for reads and mutations.** `dispatch_cli.c`
now strips `--json` for every supported read and mutating command in one table,
including `ready` (both queue and mutation forms). Every command enters JSON
mode before running and reports failures through `cli_error`, so every read and
mutating command produces the same `{schema_version, command, error: {code,
message, task_id}}` document on stderr with empty stdout. Repeated `--json` is
consistently a `usage` error everywhere.

**`doctor --json` board-load exception preserved cleanly.** `doctor --json`
already specified that a failed board load emits the doctor document with
`board: null` and a single `board_load_failed` check on stdout with exit status
1. Entering JSON mode before `load_board_or_error` captures the load error
instead of leaking text to stderr, and `doctor` suppresses the stderr error
document once the stdout check document has been written. Usage errors and
stdout write failures (`output_failed`) still emit the JSON error document on
stderr.

**Envelopes for the remaining mutating commands.** Each command reuses its
natural envelope from ADRs 0002 and 0004 rather than introducing a new shape:

- `init`, `repo set`, `normalize`, `task priority`, and `group edit` use the
  task envelope plus `result`.
- `workspace repair` uses the workspace envelope plus `result`.
- `agent create`, `agent archive`, `agent restore`, `agent session`,
  `agent command`, and `agent resume` use the agent envelope (`query.name` set
  to the agent, `query.include_archived: null`, `agents` holding the acted-on
  record) plus `warnings` and `result`.

Every command follows ADR 0002's no-op rule (`changed: false` when the command
succeeds without changing board state, such as `init` on an existing board,
`repo set` to the same path, `normalize` when nothing needed repair,
`task priority` to the current value, `group edit` with the same description,
`workspace repair` when already anchored, `agent archive|restore|session` with
no change, `agent command`, and `agent resume` when no Claude session UUID was
minted).

**Non-fatal warnings go into `warnings`, never stderr.** `cli_warning` collects
non-fatal warnings in the active JSON context using the existing
`{code, message, task_id, agent}` warning shape from `status` and `log`. Task,
workspace, and agent mutation envelopes all emit `warnings` alongside `result`.
`append_dispatch_log` records `log_append_failed` (and `workspace remove` now
appends to the log before emitting JSON, matching every other mutation),
`init` scaffolding records `scaffold_failed`, and `repo set` on a non-git
directory records `repo_not_git`. In JSON mode, a successful command therefore
always writes one JSON document on stdout and leaves stderr empty.

**Discoverability in `--help` and shell completions.** `dispatch --help` now
lists `--json` for both read and mutating commands, and the bash, zsh, and fish
completion scripts offer `--json` (along with the previously missing
subcommands `repo set`, `group edit|list|show`, `task move|priority`,
`workspace repair`, and `unassign`).

**New error codes and versioning.** Nine error codes are added to version 1
(`invalid_priority`, `invalid_agent`, `invalid_runner`, `agent_exists`,
`agent_not_found`, `agent_active`, `workspace_adopted`, `log_unreadable`, and
`io_failed`), along with three operational warning codes (`log_append_failed`,
`scaffold_failed`, `repo_not_git`). All changes are additive on `schema_version`
1: read command success shapes are unchanged, workspace mutations gain the
additive `warnings` array, and new `--json` output appears only where `--json`
was previously rejected or errored as text.

## Consequences

- Tools and agents can parse every `--json` command's success and failure
  uniformly without special-casing read errors, missing subcommands, or stray
  warning lines on stderr.
- Adding `--json` to any future command means registering it in `json_commands`,
  routing errors through `cli_error` and non-fatal warnings through
  `cli_warning`, and adding completion entries.
- Read commands that do not accept `--json` (`group list|show`,
  `commit list|show`, `repo` without `set`) remain text-only.
