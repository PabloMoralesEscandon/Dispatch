# 0004: JSON reads for agents, workspaces, and the log

- **Status:** Accepted
- **Date:** 2026-11-16

## Context

GitHub issue #2 comes from the Orca plugin, which needs to read Dispatch
state. Only the task read commands accept `--json`. A tool that wants agents,
workspaces, or the audit trail has to parse human output or read
`dispatch.json` and `dispatch.log` directly, and the README calls those files
implementation details.

The issue asks for `agent list [--all] --json`, `agent show <name> --json`,
`workspace list --json`, and `workspace show <id> --json`. It also asks for a
new read command, `dispatch log`, over `dispatch.log`, with the filters of the
TUI logs view and a `--limit`. Output should use the existing envelope
(`schema_version`, `command`, `board`, `query`, `summary`) with new top-level
`agents`, `workspaces`, or `records` arrays.

`workspace list --json` and `workspace show --json` already existed, with an
envelope of `schema_version`, `command`, `board`, `summary: {returned}`, and
`workspaces` (ADR 0002 reuses it for workspace mutations). It had no `query`.

JSON error documents for read commands are out of scope and tracked in
issue #12.

## Decision

**One envelope shape per record kind.** Agent, workspace, and log responses
share one layout: `schema_version`, `command`, `board`, `query`, `summary`,
then the record array. `board` is `{name, repo_path}`, as in the existing
workspace envelope. Task groups are not repeated, because these responses are
not about tasks. `query` holds the normalized selection, and `summary` always
has `total` (records of that kind on the board, or readable records in the
log) and `returned` (the array length), plus counts specific to the kind.

**Workspace envelope gains `query` and counts.** The workspace envelope now
carries `query: {workspace}`, the workspace ID for `workspace show` and
`null` for `workspace list`, and `summary` adds `total`, `active`, and
`removed` next to `returned`. Workspace mutations use the same envelope, so
they get the same fields. Following ADR 0002's single-target rule,
`query.workspace` is the record's ID when a mutation returns exactly one
record, and `null` otherwise. Every change is additive, so existing parsers
keep working.

**Agent objects.** Each agent object has the stored fields (`name`, `runner`,
`model`, `archived`, `agent_dir`, `prompt_path`, `run_script_path`,
`session_id`, `current_task`, `last_workspace`, `created_at`, `updated_at`),
with unset values as `null`. It also has two derived arrays, so a tool does
not need a second query to see what an agent holds: `assigned_tasks` (tasks
assigned to the agent, in board order) and `workspaces` (IDs of its
non-removed workspaces). `archived` is a boolean, the stored form, rather than
the `enabled`/`archived` word of the human output. `agent list` skips
archived agents unless `--all` is given, as in text mode. `agent show`
returns any agent, archived or not, so its `query.include_archived` is
`null`, the way `include_done` is `null` for `show`.

**`dispatch log` is a board read command.** It loads the board, which
supplies `board` and fails as other reads do outside a workflow directory,
then reads `dispatch.log` next to it. A missing log is an empty successful
read, because a board whose log was deleted is still a valid board. The
command never writes to the log. Text mode prints one line per record,
oldest first: timestamp, actor, command, action, every string target as
`key:value`, and the message.

**Filters: the TUI's fields, exact matches.** `--task`, `--actor`,
`--agent`, `--workspace`, `--command`, and `--action` compare against the
same record fields as the TUI filters: `targets.task`, `actor`,
`targets.agent` *or* `actor`, `targets.workspace`, `command`, and `action`.
That field lookup now lives in one shared function, `dispatch_log_filter_values`,
used by both. The TUI keeps its case-insensitive substring match for
interactive browsing. The CLI matches exactly and case-sensitively, because
its values are IDs: `--task DE-1` must not return `DE-10`, and a script must
not have to filter again. Filters combine with AND, each may be given once,
and an empty value is a usage error.

**`--limit N` keeps the newest N matches, newest last.** The array stays in
file order, so appending new records to an earlier result is the natural way
to follow the log. `N` must be a positive integer. `summary.matched` reports
how many records matched before the limit, so a tool can tell whether the
limit cut anything off.

**Records are normalized, not invented.** Each record has the fields the
writer produces, in a fixed order: `version`, `timestamp`, `actor`,
`command`, `action`, `targets`, `outcome`, `message`, and `context`. A
missing `targets` becomes `{}` and a missing `outcome`, `message`, or
`context` becomes `null`. Fields this reader does not know are passed through
after them, keeping the log's "ignore unknown fields" rule. Each record also
gets `line`, its 1-based line number in `dispatch.log`. The log is
append-only, so the line number is a stable cursor for a tool that wants only
newer records. `timestamp` stays the stored RFC 3339 string rather than
becoming Unix seconds like board timestamps, because the log is its own
format and a second form of the same value would add nothing.

**Bad lines are skipped and counted, never fatal.** A record is usable when
it is a JSON object with integer `version` 1 and string `timestamp`, `actor`,
`command`, and `action`, and its optional fields have the right types. Blank
lines are ignored. Any other line is skipped. Lines whose `version` is
another integer are counted as `unsupported_log_version`, and every other bad
line as `malformed_log_lines`. Each kind produces at most one warning, with
its `count` and `first_line`, in the JSON `warnings` array (the status
warning shape, plus those two fields) or as a `Warning:` line on stderr in
text mode. The exit status stays 0. Lines are read with no length limit, so
a long record is never split into fragments.

**Versioning.** `schema_version` stays 1: agent and log responses are new
documents, and the workspace envelope only gains fields.

## Consequences

- Orca can read every kind of Dispatch state through the CLI, and can follow
  the log by remembering the last `line` it saw.
- The TUI and the CLI cannot disagree about which field a filter reads, but
  they deliberately disagree about how it matches.
- Reading the log is linear in its size. `--limit` keeps memory bounded by
  trimming as it reads, but the whole file is still scanned.
- Errors from these reads, such as an unknown agent or a bad option, remain
  text on stderr with no JSON document, as for every read command, until
  issue #12.

### Open questions

- `--task` follows the TUI and reads only `targets.task`, so `dep add` and
  `dep remove` records, which name `targets.dependency` and
  `targets.dependent`, do not match it. Widening it would change the TUI
  filter too.
- `workspace show` prints whether the Git worktree is present, but the
  workspace object does not carry it. Adding it would make `workspace list`
  run Git checks for every record.
- `dispatch log` has no `--since-line` option yet. The `line` field makes one
  easy to add.
