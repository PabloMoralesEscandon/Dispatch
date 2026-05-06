# Dispatch Test Plan

This plan defines the CLI-level coverage expected for Dispatch once the core
workflow commands are implemented.

## Test Fixtures

Fixtures live under `tests/fixtures/`:

- `empty-board.json`: minimal initialized board.
- `dependency-board.json`: board with multiple groups, dependencies, blocked
  tasks, review tasks, and done tasks.

Tests should copy fixtures into a temporary directory before each case and run
the Dispatch binary from that directory, so tests never mutate repository data.

## Core Acceptance Tests

### Initialization

- `dispatch init` creates `dispatch.json` when missing.
- Running `dispatch init` again is idempotent.
- Invalid JSON produces a clear error and does not crash.

### Groups

- `group add Development` creates a group with a stable prefix.
- Duplicate group prefixes are rejected.
- Task ID generation uses the selected group prefix.

### Tasks

- `task add <group> <title>` creates a proposed task with a generated ID.
- Task titles can change without changing task IDs.
- Empty task titles are rejected.
- `show <id>` prints title, description, group, state, dependencies,
  dependents, assignment, and history.

### Dependencies

- `dep add <from> <to>` records `from` in `to.depends_on`.
- A task can depend on multiple task IDs.
- Duplicate dependencies are ignored or rejected clearly.
- Missing dependency IDs are rejected.
- Cycles are rejected.
- Reverse "blocks" relationships are computed for display.

### Derived State

- Tasks with unmet dependencies appear blocked.
- A blocked task becomes ready when every dependency is done.
- `normalize` recomputes derived blocked/ready state.
- Derived blocked state is not trusted blindly from stored JSON.

### Lifecycle

- Only ready tasks can be started.
- `start <id> --actor <actor>` sets `assigned_to`, `started_by`, `started_at`,
  and moves the task to doing.
- A second actor cannot start an already assigned doing task.
- `pause <id> --actor <actor>` clears or records assignment according to the
  final command design and returns the task to ready.
- `finish <id> --actor <actor>` records `completed_by` and `completed_at`.
- Tasks with `requires_review` move to review after finish.
- Tasks without `requires_review` move directly to done after finish.
- `review <id> --actor <actor>` moves review tasks to done.

### Delete

- Deleting a task with no dependents succeeds.
- Deleting a task that blocks other tasks is rejected unless the final design
  includes an explicit force path.
- Deleting a missing task reports a clear error.

### Views

- `list` groups tasks by group and workflow order.
- `ready` lists only startable tasks.
- `blocked` lists blockers for each blocked task.
- Review tasks are visible in a dedicated view or state filter.
- JSON output is valid JSON and contains stable field names.

## Regression Tests For Removed TDL Behavior

- Due date options are rejected or ignored with a clear compatibility message.
- Recurrence options are not part of help output.
- Notification options are not part of help output.
- Category options are not part of help output.
- Project inheritance commands are not registered.
- `clear` is not registered.
- The default storage file is `dispatch.json`, not `tasks.json`.

## Manual Smoke Test

Until the automated runner exists, every implementation task should at least:

1. Build with `./nob`.
2. Run the relevant command with `--help` or a simple fixture.
3. Confirm no repository fixture was mutated directly.
