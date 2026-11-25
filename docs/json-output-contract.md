# Dispatch JSON output contract

This document defines version 1 of the machine-readable output produced when
`--json` is passed to:

- the read commands `list`, `show`, `ready`, `blocked`, `reviews`,
  `proposed`, `status`, `doctor`, `agent list`, `agent show`,
  `workspace list`, `workspace show`, and `log`;
- the mutating commands `init`, `repo set`, `normalize`,
  `task add|edit|move|delete|priority`, `group add|ready|edit`,
  `dep add|remove`, `ready <id>`, `start`, `finish`, `review`, `unassign`,
  `commit add`, `agent create|archive|restore|session|command|resume`, and
  `workspace create|adopt|remove|repair|prune`, described in
  [Mutation results](#mutation-results).

## General rules

- A successful command writes one JSON object followed by a newline to stdout
  and nothing to stderr.
- JSON mode never emits headings, color escapes, hints, or other human text.
- Failing commands (both read and mutating) retain their existing non-zero exit
  status, write nothing to stdout, and write one JSON error document to stderr
  (see [Error documents](#error-documents)). The one exception is
  `doctor --json` when the board itself cannot be loaded, which emits its
  documented `board_load_failed` check on stdout with empty stderr.
- Non-fatal warnings that occur after a successful mutation (such as a failed
  `dispatch.log` append) are placed in the document's `warnings` array rather
  than printed as text on stderr.
- `--json` may appear anywhere after the command name, or after the subcommand
  for commands with subcommands (`agent`, `workspace`, `task`, `group`, `dep`,
  `commit`, `repo set`). `ready` without a task ID is the read-only queue form;
  `ready <id>... --json` is the mutating form.
- `--json` may be given once. A repeated `--json` is a `usage` error.
- Without `--json`, command arguments and human-readable output are unchanged.
- New optional fields may be added in a compatible release. Removing a field,
  changing its type, or changing its meaning requires a schema version change.

## Response envelope

Task commands return these top-level fields. Agent, workspace, and log
responses use the same `schema_version`, `command`, `board`, `query`, and
`summary` fields with their own record array; see
[Workspace responses](#workspace-responses),
[Agent responses](#agent-responses), and [Log responses](#log-responses).

```json
{
  "schema_version": 1,
  "command": "list",
  "board": {
    "name": "Dispatch",
    "repo_path": "repo",
    "groups": [
      {"id": "development", "name": "Development", "prefix": "DE"}
    ]
  },
  "query": {
    "task_id": null,
    "group": null,
    "include_done": false,
    "states": []
  },
  "summary": {
    "total": 2,
    "returned": 1,
    "states": {
      "proposed": 0,
      "ready": 1,
      "blocked": 1,
      "doing": 0,
      "review": 0,
      "done": 0,
      "paused": 0
    },
    "agents": {"enabled": 1, "archived": 0, "with_current_task": 1},
    "workspaces": {"active": 1, "removed": 0}
  },
  "tasks": [],
  "warnings": []
}
```

`schema_version` is the integer `1`. `command` is the invoked command name.
`board.groups` is always the complete group list in board order, including
groups with no returned tasks. `repo_path` is the configured path, or `.` when
none is configured.

`query` records the normalized selection applied by the command:

- `task_id` is a task ID for `show`, otherwise `null`.
- `group` is the resolved group ID for a filtered `list`, otherwise `null`.
- `include_done` is `true` only for `list all`; it is `null` when the option is
  not meaningful to a command.
- `states` contains presentation-state names selected by queue commands. It is
  empty when no state filter applies.

`summary.total` and every `summary.states` count cover the complete board,
regardless of the command filter. `summary.returned` equals the length of
`tasks`. State counts use presentation state, defined below. Agent and
workspace counts follow the existing `status` command: a workspace is active
unless its stored state is `removed`.

Each `board.groups` entry contains `id`, `name`, `prefix`, and
`description` (the group's recorded scope, empty string when unset).

## Task object

Every entry in `tasks` has the same shape:

```json
{
  "id": "DE-02",
  "title": "Implement JSON output",
  "description": "Add machine-readable output.",
  "group": "development",
  "state": "blocked",
  "stored_state": "blocked",
  "requires_review": true,
  "assigned_to": null,
  "started_by": null,
  "completed_by": null,
  "depends_on": ["DE-01"],
  "blocked_by": ["DE-01"],
  "blocks": ["DE-03"],
  "commits": [],
  "issue": {
    "repo": "acme/widgets",
    "number": 12,
    "url": "https://github.com/acme/widgets/issues/12"
  },
  "workspace": null,
  "created_at": 1750000000,
  "started_at": null,
  "completed_at": null,
  "updated_at": 1750000100,
  "history": [
    {
      "actor": "user",
      "action": "ready",
      "note": "",
      "timestamp": 1750000100
    }
  ]
}
```

`state` is the presentation state shown by the CLI. A stored `proposed` task
remains `proposed`; otherwise this is the effective state after dependencies
are evaluated. `stored_state` is the persisted lifecycle state and can differ
from `state` when derived state has not yet been normalized and persisted.

`depends_on` contains all direct dependency IDs. `blocked_by` contains only
dependencies that are not effectively done. `blocks` contains IDs of all tasks
that directly depend on this task. `commits` contains recorded commit refs.

`issue` is the task's linked GitHub issue, or `null` when the task has none
(including boards written before issue links existed). When set it contains:

| Field | Type | Meaning |
| --- | --- | --- |
| `repo` | string | `owner/repo` as linked, case preserved |
| `number` | integer | Issue number, always positive |
| `url` | string | `https://github.com/<repo>/issues/<number>` |

A task links at most one issue. The field was added in a compatible release,
so `schema_version` stays `1`; consumers written before it should already
ignore it as an unknown field. Dispatch never contacts GitHub to fill it, so
the issue is not guaranteed to exist.

`workspace` is `null` when no active workspace covers the task. Otherwise it
contains:

```json
{
  "id": "DE-02",
  "actor": "codex",
  "path": "/work/Dispatch-agent-codex-DE-02",
  "branch": "agent/codex/DE-02",
  "state": "active",
  "adopted": false,
  "external_id": null
}
```

`adopted` and `external_id` mean the same as in a
[workspace record](#workspace-responses).

Actor fields and unset timestamps are JSON `null`, not empty strings or `-`.
Set timestamps are Unix seconds. Task, dependency, commit, and history arrays
preserve board or stored order; `blocks` follows board task order.

## Command selections

| Command | Tasks returned | Query values |
| --- | --- | --- |
| `list` | All non-done tasks | `include_done: false` |
| `list all` | All tasks | `include_done: true` |
| `list [all] <group>` | The corresponding list selection in the resolved group | `group: <group-id>` |
| `show <id>` | The selected task as a one-element array | `task_id: <id>`, `include_done: null` |
| `ready` | Tasks in presentation state `ready` | `states: ["ready"]`, `include_done: null` |
| `blocked` | Tasks in presentation state `blocked` | `states: ["blocked"]`, `include_done: null` |
| `reviews` | Tasks in presentation state `review` | `states: ["review"]`, `include_done: null` |
| `proposed` | Tasks in presentation state `proposed` | `states: ["proposed"]`, `include_done: null` |
| `status` | Tasks in presentation state `ready` or `review` | `states: ["ready", "review"]`, `include_done: null` |

All task selections retain board order. An empty successful selection returns
`"tasks": []`; informational empty-queue hints from human output are omitted.

## Status and operational warnings

Among read commands, `warnings` is empty for every task command except `status`
(and `log` reports skipped-line warnings in its own `warnings` array). `status`
emits structured versions of its existing diagnostics in board order:

```json
{
  "code": "missing_commits",
  "message": "DE-01 has no recorded commits",
  "task_id": "DE-01",
  "agent": null
}
```

The version 1 warning codes are:

- `missing_commits` for a done or review task with no recorded commits.
- `assignment_state_mismatch` for an assigned task outside doing or review.
- `missing_current_task` for an agent session that references an unknown task.
- `duplicate_issue` for a task linking the same issue as an earlier task in
  board order. Repos compare case-insensitively. `task_id` names the later
  task, and the message names the earlier one.

Each warning always contains `code`, `message`, `task_id`, and `agent`. Fields
that do not apply are `null`.

Mutating commands also populate `warnings` with the same object shape whenever
a non-fatal warning occurs after a successful mutation, instead of printing
text to stderr:

- `log_append_failed` when appending to `dispatch.log` fails after the board
  has been saved.
- `scaffold_failed` when `init` cannot create `.dispatch` directories or
  workflow instruction files (`AGENTS.md`, `CLAUDE.md`).
- `repo_not_git` when `repo set` points at a directory that is not a Git
  repository.

## Workspace responses

`workspace list --json` and `workspace show <id> --json` use a workspace
envelope instead of the task envelope:

```json
{
  "schema_version": 1,
  "command": "workspace list",
  "board": { "name": "Dispatch", "repo_path": "repo" },
  "query": { "workspace": null },
  "summary": { "total": 2, "returned": 1, "active": 1, "removed": 1 },
  "workspaces": [ ... ]
}
```

`workspace list` returns every non-removed record in board order;
`workspace show` returns the selected record as a one-element array. An empty
successful listing returns `"workspaces": []`.

`query.workspace` is the ID of the record `workspace show` selected (the
argument may name its task instead), and `null` for `workspace list`.
`summary.returned` is the length of `workspaces`; `total`, `active`, and
`removed` count every stored workspace record, where a record is active
unless its state is `removed`, as in the task envelope's
`summary.workspaces`. `query` and the `summary` counts other than `returned`
were added in a compatible release.

Each workspace record contains:

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | string | Workspace identifier (normally the anchor task ID) |
| `task_id` | string | Task the workspace was created for |
| `task_state` | string or null | Effective state of that task, `null` if the task is missing |
| `actor` | string | Owning agent |
| `path` | string | Worktree path |
| `branch` | string | Worktree branch |
| `repo_path` | string | Repository the worktree was created from |
| `state` | string | `creating`, `active`, or `removed` |
| `sequence_tasks` | array of string | Task chain for `--sequence` workspaces, else `[]` |
| `review_gate` | string or null | Sequence-terminating review task, if any |
| `adopted` | boolean | `true` when `workspace adopt` recorded a worktree another tool created |
| `external_id` | string or null | `<tool>:<id>` given with `--external`, else `null` |
| `created_at` | integer or null | Unix timestamp |
| `updated_at` | integer or null | Unix timestamp |

`adopted` and `external_id` were added in a compatible release, so
`schema_version` stays `1`. Records written before them report `false` and
`null`. Dispatch never deletes an adopted worktree unless
`workspace remove --delete-worktree` asks it to.

## Agent responses

`agent list [--all] --json` and `agent show <name> --json` use an agent
envelope:

```json
{
  "schema_version": 1,
  "command": "agent list",
  "board": { "name": "Dispatch", "repo_path": "repo" },
  "query": { "name": null, "include_archived": false },
  "summary": {
    "total": 2,
    "returned": 1,
    "enabled": 1,
    "archived": 1,
    "with_current_task": 1
  },
  "agents": [
    {
      "name": "codex",
      "runner": "codex",
      "model": null,
      "archived": false,
      "agent_dir": ".dispatch/agents/codex",
      "prompt_path": ".dispatch/agents/codex/codex-PROMPT.md",
      "run_script_path": ".dispatch/agents/codex/run.sh",
      "session_id": null,
      "current_task": "DE-02",
      "last_workspace": "DE-02",
      "assigned_tasks": ["DE-02"],
      "workspaces": ["DE-02"],
      "created_at": 1750000000,
      "updated_at": 1750000100
    }
  ]
}
```

`agent list` returns agents in board order, skipping archived agents unless
`--all` is given; `query.include_archived` records which. `agent show`
returns the named agent, archived or not, as a one-element array, with
`query.name` set and `query.include_archived` `null`. An empty listing
returns `"agents": []`.

`summary.returned` is the length of `agents`. `total`, `enabled`, `archived`,
and `with_current_task` count every agent on the board, with the same
meaning as the task envelope's `summary.agents`.

Each agent object contains:

| Field | Type | Meaning |
| --- | --- | --- |
| `name` | string | Agent name, also its actor name |
| `runner` | string | `codex`, `claude`, or `opencode` |
| `model` | string or null | Model passed to the runner |
| `archived` | boolean | `true` when the agent is archived |
| `agent_dir` | string or null | Support directory, relative to the workflow directory |
| `prompt_path` | string or null | Generated prompt file |
| `run_script_path` | string or null | Generated run script, `null` with `--no-run-script` |
| `session_id` | string or null | Recorded runner session ID |
| `current_task` | string or null | Task recorded with `agent session --current-task` |
| `last_workspace` | string or null | Workspace recorded with `agent session --last-workspace` |
| `assigned_tasks` | array of string | IDs of tasks assigned to the agent, in board order |
| `workspaces` | array of string | IDs of the agent's non-removed workspaces, in board order |
| `created_at` | integer or null | Unix timestamp |
| `updated_at` | integer or null | Unix timestamp |

`current_task` and `last_workspace` are what the agent recorded and may name
a task or workspace that no longer exists; `status` warns about a missing
current task. `assigned_tasks` and `workspaces` are derived from the board
when the command runs.

## Log responses

`dispatch log --json` reads `dispatch.log`, the audit trail described in the
README, and returns its records:

```json
{
  "schema_version": 1,
  "command": "log",
  "board": { "name": "Dispatch", "repo_path": "repo" },
  "query": {
    "task": "DE-01",
    "actor": null,
    "agent": null,
    "workspace": null,
    "command": null,
    "action": null,
    "limit": 1
  },
  "summary": { "total": 9, "matched": 3, "returned": 1, "skipped": 1 },
  "records": [
    {
      "line": 6,
      "version": 1,
      "timestamp": "2026-06-19T12:35:20Z",
      "actor": "codex",
      "command": "start",
      "action": "start",
      "targets": { "task": "DE-01" },
      "outcome": "success",
      "message": "Started DE-01",
      "context": { "previous_state": "ready", "new_state": "doing" }
    }
  ],
  "warnings": [
    {
      "code": "malformed_log_lines",
      "message": "skipped 1 malformed line in dispatch.log, first at line 7",
      "task_id": null,
      "agent": null,
      "count": 1,
      "first_line": 7
    }
  ]
}
```

The command loads the board first and fails like other read commands outside
a workflow directory. A missing `dispatch.log` is not an error: it returns
`"records": []`.

Filters select records by exact, case-sensitive comparison and combine with
AND. Each may be given once:

| Option | `query` key | Matches records whose |
| --- | --- | --- |
| `--task <id>` | `task` | `targets.task` equals the value |
| `--actor <name>` | `actor` | `actor` equals the value |
| `--agent <name>` | `agent` | `targets.agent` or `actor` equals the value |
| `--workspace <id>` | `workspace` | `targets.workspace` equals the value |
| `--command <name>` | `command` | `command` equals the value |
| `--action <name>` | `action` | `action` equals the value |

These are the fields the TUI logs view filters on; the TUI matches
substrings ignoring case, the CLI does not. Unused filters are `null` in
`query`.

`--limit <n>` keeps the newest `n` matching records; `query.limit` is `n`,
or `null` without the option. `n` must be a positive integer. `records` is
always in file order, oldest first, so the newest record is last.

`summary.total` counts the readable records in the log, `matched` those that
pass the filters, `returned` the length of `records` (`matched` cut to the
limit), and `skipped` the lines that were not readable records.

Each record contains:

| Field | Type | Meaning |
| --- | --- | --- |
| `line` | integer | 1-based line number in `dispatch.log` |
| `version` | integer | Log record version, always `1` |
| `timestamp` | string | UTC time in RFC 3339 form, as stored |
| `actor` | string | Actor responsible for the change |
| `command` | string | Command family, such as `start` or `workspace` |
| `action` | string | Action within the command, such as `workspace_create` |
| `targets` | object | Stable IDs such as `task`, `group`, `agent`, `workspace`; `{}` when absent |
| `outcome` | string or null | `success` for records Dispatch writes |
| `message` | string or null | Short human-readable summary |
| `context` | object or null | Structured details, `null` when absent |

Any other field of a stored record follows these, unchanged. The log is
append-only, so `line` identifies a record across reads; a tool can follow
the log by keeping the last `line` it saw.

A line is a readable record when it is a JSON object whose `version` is the
integer `1`, whose `timestamp`, `actor`, `command`, and `action` are strings,
and whose `targets` and `context` are objects and `outcome` and `message`
strings when present. Blank lines are ignored. Other lines are skipped and
reported in `warnings`, at most one warning per code, in this order:

| Code | Skipped lines |
| --- | --- |
| `malformed_log_lines` | Not JSON, not an object, or missing or mistyped fields |
| `unsupported_log_version` | An object whose `version` is an integer other than `1` |

Log warnings have the status warning fields (`code`, `message`, `task_id`,
`agent`, the last two always `null`) plus `count`, the number of lines
skipped, and `first_line`, the line number of the first. Skipped lines never
make the command fail. Without `--json`, the same messages are printed to
stderr as `Warning: ...` lines and the exit status is still 0.

## Doctor responses

`doctor --json` emits every diagnostic check in run order:

```json
{
  "schema_version": 1,
  "command": "doctor",
  "board": { "name": "Dispatch", "repo_path": "repo" },
  "summary": { "warnings": 2 },
  "checks": [
    {
      "status": "ok",
      "kind": null,
      "subject": null,
      "message": "board loaded",
      "fix": null
    },
    {
      "status": "warn",
      "kind": "missing_commits",
      "subject": "DE-01",
      "message": "done task DE-01 has no recorded commits",
      "fix": "record one with dispatch commit add <id> <sha>"
    }
  ]
}
```

Every check has `status` (`ok` or `warn`), `message`, and — for warnings —
a machine-stable `kind`, an optional `subject` (the task ID, agent name,
workspace ID, or shell the warning refers to), and a `fix` hint. `ok` checks
carry `null` for `kind`, `subject`, and `fix`. If the board itself cannot be
loaded, `board` is `null` and the single `board_load_failed` warning is
emitted on stdout with exit status 1 and empty stderr. Usage errors (such as
extra arguments or a repeated `--json`) emit a JSON error document on stderr
like other read commands.

The version 1 doctor warning kinds are `board_load_failed`, `repo_not_git`,
`agents_md_missing`, `dispatch_not_on_path`, `workflow_entry_missing`,
`workflow_entry_unexpected_type`, `completion_home_unset`,
`completion_missing`, `agent_prompt_missing`,
`agent_run_script_not_executable`, `missing_current_task`,
`workspace_path_missing`, `workspace_path_moved`, `orphaned_workspace`, `missing_commits`,
`dep_self`, `dep_missing_reference`, `dep_duplicate`,
`assignment_state_mismatch`, and `start_provenance_mismatch`. Kinds shared
with status warnings (`missing_commits`, `missing_current_task`,
`assignment_state_mismatch`) describe the same conditions.

## Mutation results

A mutating command passed `--json` writes one JSON document to stdout after the
change has been saved. Human confirmation lines such as `Started DE-01` are
not printed. Workflow state, task history, and `dispatch.log` records are
identical to the text form.

Task and board commands (`init`, `repo set`, `normalize`,
`task add|edit|move|delete|priority`, `group add|ready|edit`,
`dep add|remove`, `ready <id>`, `start`, `finish`, `review`, `unassign`, and
`commit add`) use the [task envelope](#response-envelope), with:

- `command` set to the full command name, such as `"task add"` or `"start"`;
- `tasks` holding the target tasks that still exist after the change, read
  from the saved board, in target order (`task delete`, `group add|edit`,
  `init`, `repo set`, and `normalize` return `[]`);
- `query.task_id` set to the target ID when there is exactly one target,
  otherwise `null`; `query.group` set to the group ID for `group add`,
  `group ready`, and `group edit`, otherwise `null`; `query.include_done`
  `null`; and `query.states` `[]`;
- `summary` describing the board after the change; and
- `warnings` holding any non-fatal warnings that occurred during the command,
  otherwise `[]`.

Workspace commands (`workspace create|adopt|remove|repair|prune`) use the
[workspace envelope](#workspace-responses), whose `workspaces` array holds the
records the command acted on, plus `warnings` and `result`. A record the
command removed is reported as it was just before removal, with `state` set to
`"removed"`. `query.workspace` is that record's ID when exactly one record is
returned, otherwise `null`, and `summary` describes the board after the change.

Agent commands (`agent create|archive|restore|session|command|resume`) use the
[agent envelope](#agent-responses), with `query.name` set to the agent name,
`query.include_archived` `null`, `agents` holding the agent record after the
command as a one-element array, `summary` describing the board after the
change, plus `warnings` and `result`.

All three envelopes then carry a `result` object:

```json
"result": {
  "action": "finish",
  "changed": true,
  "task": "DE-01",
  "previous_state": "doing",
  "new_state": "done",
  "targets": [
    {"task": "DE-01", "previous_state": "doing", "new_state": "done"}
  ],
  "newly_ready": ["DE-02"],
  "review_required": false
}
```

Every result contains these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `action` | string | Stable action name, from the table below |
| `changed` | boolean | `false` when the command succeeded without changing the board |
| `task` | string or null | The target task when there is exactly one, else `null` |
| `previous_state` | string or null | State of `task` before the command; `null` if it did not exist or there is no single target |
| `new_state` | string or null | State of `task` after the command; `null` if it no longer exists or there is no single target |
| `targets` | array | One `{task, previous_state, new_state}` entry per target task, in command order, with the same `null` rules |
| `newly_ready` | array of string | Task IDs, in board order, whose state is `ready` after the command and was not `ready` (or did not exist) before |

All states are presentation states, as defined under
[Task object](#task-object). `newly_ready` is computed by comparing the whole
board before and after the command, so it includes a target the command made
ready itself (`ready DE-01`) as well as dependents unblocked by `finish`,
`review`, `dep remove`, or `task delete --force`.

Each action may add the fields listed here. Consumers must ignore fields they
do not know.

| Command | `action` | Targets | Extra fields |
| --- | --- | --- | --- |
| `init` | `init` | None | `created` (boolean), `repo_path` (string) |
| `repo set` | `repo_set` | None | `previous_repo_path` (string), `repo_path` (string), `is_git_repo` (boolean) |
| `normalize` | `normalize` | None | `state_updates`, `assignment_repairs`, `session_updates`, `prompt_updates` (integers) |
| `task add` | `task_add` | The new task | — |
| `task edit` | `task_edit` | The edited task | — |
| `task move` | `task_move` | The moved task | `from_group`, `to_group` (group IDs) |
| `task delete` | `task_delete` | The deleted task | `force` (boolean) |
| `task priority` | `task_priority` | The updated task | `previous_priority`, `priority` (integers) |
| `group add` | `group_add` | None | `group` (new group ID) |
| `group ready` | `group_ready` | Tasks readied, then tasks whose review gate was cleared | `group` (group ID), `no_review` (boolean) |
| `group edit` | `group_edit` | None | `group` (group ID), `description` (string) |
| `dep add` | `dependency_add` | The dependent | `dependency`, `dependent` (task IDs) |
| `dep remove` | `dependency_remove` | The dependent | `dependency`, `dependent` (task IDs) |
| `ready <id>...` | `ready` | Each named task | — |
| `start` | `start` | The started task | — |
| `finish` | `finish` | The finished task | `review_required` (boolean) |
| `review` | `review` | The accepted task | — |
| `unassign` | `unassign` | The unassigned task | `previous_assignee` (string or null) |
| `commit add` | `commit_add` | The task | `commit` (the recorded ref) |
| `agent create` | `agent_create` | None | `agent` (name), `scratch_dir`, `decisions_dir`, `command` (strings) |
| `agent archive` | `agent_archive` | None | `agent` (name), `archived` (boolean, `true`) |
| `agent restore` | `agent_restore` | None | `agent` (name), `archived` (boolean, `false`) |
| `agent session` | `agent_session` | None | `agent` (name) |
| `agent command` | `agent_command` | None | `agent` (name), `command` (string) |
| `agent resume` | `agent_resume` | None | `agent` (name), `command` (string), `resume_script_path` (string or null), `session_generated` (boolean) |
| `workspace create` | `workspace_create` | The workspace's task | `workspace` (workspace ID) |
| `workspace adopt` | `workspace_adopt` | The workspace's task | `workspace` (workspace ID), `external_id` (string or null) |
| `workspace remove` | `workspace_remove` | The workspace's task | `workspace` (workspace ID), `force` (boolean), `worktree_deleted` (boolean: `false` when only the record of an adopted workspace was cleared) |
| `workspace repair` | `workspace_repair` | The workspace's task | `workspace` (workspace ID), `old_path`, `path`, `old_repo_path`, `repo_path` (strings) |
| `workspace prune` | `workspace_prune` | None | `dry_run` (boolean), `skipped` (array) |

`changed` is `false` for successful no-ops: `init` when `dispatch.json` already
exists, `repo set` to the current path, `normalize` when no task state,
assignment, session ID, or agent prompt needed repair, `task priority` to the
task's current priority, `group edit` with the existing description,
`commit add` of a ref the task already records, `dep add` of an existing
dependency, `task edit` that only re-links the current issue or clears an
absent one, `group ready` that readies nothing and clears no review gate,
`agent archive` / `restore` / `session` that leave the record unchanged,
`agent command`, `agent resume` when no Claude session UUID was generated,
`workspace repair` when the workspace is already anchored, and `workspace prune`
that removes nothing (always, with `--dry-run`). Every other success reports
`true`.

`workspace prune` lists the pruned records in `workspaces` (with `--dry-run`,
the records that would be pruned, in their current state) and every
candidate it left alone in `skipped`. Pruned records with `adopted: true` had
only their record removed; their worktrees were left in place. Adopted
records are never skipped:

```json
"skipped": [{"workspace": "DE-07", "reason": "dirty"}]
```

| `reason` | Meaning |
| --- | --- |
| `worktree_exists` | A `--stale` candidate whose git worktree still exists |
| `not_a_worktree` | A `--done` candidate whose path is not a registered git worktree |
| `dirty` | A `--done` candidate with uncommitted changes |
| `remove_failed` | `git worktree remove` failed for a `--done` candidate |

## Error documents

When any command passed `--json` (read or mutating) fails, it keeps its
existing non-zero exit status, writes nothing to stdout, and writes one JSON
document to stderr in place of the human error text (the only exception is
`doctor --json` failing to load the board, which emits `board_load_failed` on
stdout and leaves stderr empty):

```json
{
  "schema_version": 1,
  "command": "start",
  "error": {
    "code": "invalid_state",
    "message": "Cannot start DE-02: task is blocked, not ready",
    "task_id": "DE-02"
  }
}
```

`code` is a stable identifier from the list below. `message` is the human
error text without its trailing newline; when the text form prints several
lines (for example an error followed by a usage line), they are joined with
`\n`. Messages are meant for people and may change, so match on `code`.
`task_id` names the task the error is about, or is `null`. When a command
reports several errors, the first one sets `code` and `task_id`.

Non-fatal warnings that are not the cause of a failure, such as a failed
`dispatch.log` append after a successful save, are not printed to stderr in
JSON mode; they are returned in the document's `warnings` array.

The version 1 error codes are:

| Code | Meaning |
| --- | --- |
| `usage` | Missing, unknown, or repeated arguments or options |
| `board_missing` | No `dispatch.json` in the current directory |
| `board_locked` | The board lock could not be acquired |
| `board_unreadable` | The board could not be read or failed validation |
| `board_save_failed` | The changed board could not be saved |
| `log_unreadable` | `dispatch.log` could not be opened or read |
| `io_failed` | A local support directory or file (agent prompt, run script, resume script) could not be created or written |
| `output_failed` | The JSON result could not be written to stdout |
| `internal_error` | An unexpected failure, such as running out of memory |
| `invalid_actor` | The actor name is not valid |
| `invalid_title` | The task title is empty or contains a Dispatch task ID |
| `invalid_priority` | The `task priority` value is not an integer between -99 and 99 |
| `invalid_issue` | The `--issue` value is not a valid GitHub issue reference |
| `issue_unresolvable` | A bare issue number cannot be resolved: no GitHub `origin` remote |
| `invalid_commit` | The commit ref is not a 4–64 character hex SHA |
| `invalid_group` | The group name is empty or too long |
| `invalid_prefix` | The group prefix is not valid |
| `invalid_agent` | The agent name is empty, too long, or contains invalid characters |
| `invalid_runner` | The agent runner is not one of the supported runners |
| `invalid_path` | The path argument does not exist or cannot be resolved |
| `invalid_external_id` | The `--external` value is not `<tool>:<id>` |
| `task_not_found` | A named task does not exist |
| `group_not_found` | A named group does not exist |
| `agent_not_found` | A named agent does not exist |
| `workspace_not_found` | No workspace matches the task or workspace ID |
| `group_exists` | A group with that name or prefix already exists |
| `agent_exists` | An agent with that name already exists |
| `invalid_state` | The task's state does not allow the transition |
| `already_assigned` | The task is assigned to another actor |
| `not_assigned` | `unassign` on a task with no assignee |
| `agent_archived` | The actor or agent is archived |
| `agent_active` | `agent archive` on an agent with an active task or active workspace |
| `workspace_owner_mismatch` | The task's workspace belongs to another actor |
| `already_in_group` | `task move` to the task's current group |
| `has_dependents` | `task delete` without `--force` on a task other tasks depend on |
| `dependency_cycle` | The dependency would depend a task on itself or create a cycle |
| `dependency_not_found` | `dep remove` of a dependency that is not recorded |
| `workspace_exists` | The task already has an active workspace |
| `workspace_adopted` | `workspace repair` on an adopted workspace |
| `invalid_sequence` | The tasks do not form a valid `--sequence` chain |
| `workspace_conflict` | The workspace path, branch, or external ID is already in use, or changed concurrently |
| `repo_not_git` | The board repository is not a git repository |
| `workspace_dirty` | `workspace remove` without `--force` on a worktree with changes |
| `not_a_worktree` | The workspace path is not a registered git worktree (for `adopt`, of the board repository) |
| `branch_mismatch` | `adopt --branch` differs from the worktree's checked-out branch, or the worktree has a detached HEAD and no `--branch` was given |
| `git_failed` | A git command needed by the workspace operation failed |

New codes may be added in a compatible release; treat an unknown code as a
generic failure.
