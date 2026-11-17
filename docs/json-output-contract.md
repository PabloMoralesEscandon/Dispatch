# Dispatch JSON output contract

This document defines version 1 of the machine-readable output produced when
`--json` is passed to:

- the read commands `list`, `show`, `ready`, `blocked`, `reviews`,
  `proposed`, `status`, `doctor`, `workspace list`, and `workspace show`;
- the mutating commands `task add|edit|move|delete`, `group add|ready`,
  `dep add|remove`, `ready <id>`, `start`, `finish`, `review`, `unassign`,
  `commit add`, and `workspace create|remove|prune`, described in
  [Mutation results](#mutation-results).

## General rules

- A successful command writes one JSON object followed by a newline to stdout.
- JSON mode never emits headings, color escapes, hints, or other human text.
- Read command errors retain the existing non-zero exit status and text on
  stderr. They do not produce a partial JSON document on stdout.
- Mutating command errors retain the existing non-zero exit status, write
  nothing to stdout, and write one JSON error document to stderr (see
  [Error documents](#error-documents)).
- For read commands, `--json` may appear anywhere after the command name. For
  mutating commands it may appear anywhere after the subcommand, or after the
  command name for `ready`, `start`, `finish`, `review`, and `unassign`
  (`task add DE Title --json`, `start --json DE-01 --actor a`). `ready` without a
  task ID is the read-only queue form; `ready <id>... --json` is the mutating
  form.
- `--json` may be given once. A repeated `--json` on a mutating command is a
  `usage` error.
- Without `--json`, command arguments and human-readable output are unchanged.
- New optional fields may be added in a compatible release. Removing a field,
  changing its type, or changing its meaning requires a schema version change.

## Response envelope

Every command returns the same top-level fields:

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
  "state": "active"
}
```

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

## Status warnings

`warnings` is empty for every command except `status`. Status emits structured
versions of its existing diagnostics in board order:

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

## Workspace responses

`workspace list --json` and `workspace show <id> --json` use a workspace
envelope instead of the task envelope:

```json
{
  "schema_version": 1,
  "command": "workspace list",
  "board": { "name": "Dispatch", "repo_path": "repo" },
  "summary": { "returned": 1 },
  "workspaces": [ ... ]
}
```

`workspace list` returns every non-removed record in board order;
`workspace show` returns the selected record as a one-element array. An empty
successful listing returns `"workspaces": []`.

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
| `created_at` | integer or null | Unix timestamp |
| `updated_at` | integer or null | Unix timestamp |

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
emitted with exit status 1.

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

Task commands (`task add|edit|move|delete`, `group add|ready`,
`dep add|remove`, `ready <id>`, `start`, `finish`, `review`, `unassign`, and
`commit add`) use the [task envelope](#response-envelope), with:

- `command` set to the full command name, such as `"task add"` or `"start"`;
- `tasks` holding the target tasks that still exist after the change, read
  from the saved board, in target order (`task delete` returns `[]`);
- `query.task_id` set to the target ID when there is exactly one target,
  otherwise `null`; `query.group` set to the group ID for `group add` and
  `group ready`, otherwise `null`; `query.include_done` `null`; and
  `query.states` `[]`;
- `summary` describing the board after the change; and
- `warnings` always `[]`.

Workspace commands (`workspace create|remove|prune`) use the
[workspace envelope](#workspace-responses), whose `workspaces` array holds the
records the command acted on. A record the command removed is reported as it
was just before removal, with `state` set to `"removed"`.

Both envelopes then carry a `result` object:

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
| `task add` | `task_add` | The new task | — |
| `task edit` | `task_edit` | The edited task | — |
| `task move` | `task_move` | The moved task | `from_group`, `to_group` (group IDs) |
| `task delete` | `task_delete` | The deleted task | `force` (boolean) |
| `group add` | `group_add` | None | `group` (new group ID) |
| `group ready` | `group_ready` | Tasks readied, then tasks whose review gate was cleared | `group` (group ID), `no_review` (boolean) |
| `dep add` | `dependency_add` | The dependent | `dependency`, `dependent` (task IDs) |
| `dep remove` | `dependency_remove` | The dependent | `dependency`, `dependent` (task IDs) |
| `ready <id>...` | `ready` | Each named task | — |
| `start` | `start` | The started task | — |
| `finish` | `finish` | The finished task | `review_required` (boolean) |
| `review` | `review` | The accepted task | — |
| `unassign` | `unassign` | The unassigned task | `previous_assignee` (string or null) |
| `commit add` | `commit_add` | The task | `commit` (the recorded ref) |
| `workspace create` | `workspace_create` | The workspace's task | `workspace` (workspace ID) |
| `workspace remove` | `workspace_remove` | The workspace's task | `workspace` (workspace ID), `force` (boolean) |
| `workspace prune` | `workspace_prune` | None | `dry_run` (boolean), `skipped` (array) |

`changed` is `false` for successful no-ops: `commit add` of a ref the task
already records, `dep add` of an existing dependency, `task edit` that only
re-links the current issue or clears an absent one, `group ready` that
readies nothing and clears no review gate, and `workspace prune` that removes
nothing (always, with `--dry-run`). Every other success reports `true`.

`workspace prune` lists the pruned records in `workspaces` (with `--dry-run`,
the records that would be pruned, in their current state) and every
candidate it left alone in `skipped`:

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

When a mutating command passed `--json` fails, it keeps its existing non-zero
exit status, writes nothing to stdout, and writes one JSON document to stderr
in place of the human error text:

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

A non-fatal warning that is not the cause of the failure, such as a failure
to append to `dispatch.log` after a successful save, may still be printed to
stderr as text.

The version 1 error codes are:

| Code | Meaning |
| --- | --- |
| `usage` | Missing, unknown, or repeated arguments or options |
| `board_missing` | No `dispatch.json` in the current directory |
| `board_locked` | The board lock could not be acquired |
| `board_unreadable` | The board could not be read or failed validation |
| `board_save_failed` | The changed board could not be saved |
| `output_failed` | The JSON result could not be written to stdout |
| `internal_error` | An unexpected failure, such as running out of memory |
| `invalid_actor` | The actor name is not valid |
| `invalid_title` | The task title is empty or contains a Dispatch task ID |
| `invalid_issue` | The `--issue` value is not a valid GitHub issue reference |
| `issue_unresolvable` | A bare issue number cannot be resolved: no GitHub `origin` remote |
| `invalid_commit` | The commit ref is not a 4–64 character hex SHA |
| `invalid_group` | The group name is empty or too long |
| `invalid_prefix` | The group prefix is not valid |
| `invalid_path` | The workspace `--dir` cannot be resolved |
| `task_not_found` | A named task does not exist |
| `group_not_found` | A named group does not exist |
| `workspace_not_found` | No workspace matches the task or workspace ID |
| `group_exists` | A group with that name or prefix already exists |
| `invalid_state` | The task's state does not allow the transition |
| `already_assigned` | The task is assigned to another actor |
| `not_assigned` | `unassign` on a task with no assignee |
| `agent_archived` | The actor is an archived agent |
| `workspace_owner_mismatch` | The task's workspace belongs to another actor |
| `already_in_group` | `task move` to the task's current group |
| `has_dependents` | `task delete` without `--force` on a task other tasks depend on |
| `dependency_cycle` | The dependency would depend a task on itself or create a cycle |
| `dependency_not_found` | `dep remove` of a dependency that is not recorded |
| `workspace_exists` | The task already has an active workspace |
| `invalid_sequence` | The tasks do not form a valid `--sequence` chain |
| `workspace_conflict` | The workspace path or branch is already in use |
| `repo_not_git` | The board repository is not a git repository |
| `workspace_dirty` | `workspace remove` without `--force` on a worktree with changes |
| `not_a_worktree` | The workspace path is not a registered git worktree |
| `git_failed` | A git command needed by the workspace operation failed |

New codes may be added in a compatible release; treat an unknown code as a
generic failure.

Read commands report errors as text even with `--json`. Mutating commands
outside the list above, such as `task priority`, `group edit`,
`workspace repair`, `agent`, `normalize`, and `init`, do not accept `--json`
and report errors as text.
