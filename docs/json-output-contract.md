# Dispatch JSON output contract

This document defines version 1 of the machine-readable output produced by the
read commands `list`, `show`, `ready`, `blocked`, `reviews`, `proposed`, and
`status` when passed `--json`.

## General rules

- A successful command writes one JSON object followed by a newline to stdout.
- JSON mode never emits headings, color escapes, hints, or other human text.
- Errors retain the existing non-zero exit status and text on stderr. They do
  not produce a partial JSON document on stdout.
- `--json` may appear anywhere after the command name. For `ready`, it selects
  the read-only queue form; it is not valid with a task ID.
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

Each warning always contains `code`, `message`, `task_id`, and `agent`. Fields
that do not apply are `null`.
