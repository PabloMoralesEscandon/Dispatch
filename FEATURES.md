# Dispatch Feature Plan

Dispatch is a command-line workflow board for humans and agents. It replaces
the current TDL personal todo model with a task planning system that can track
ownership, dependencies, review gates, and grouped sequences of work.

## Current Baseline

The existing project is still TDL, a personal todo CLI. It currently supports:

- Task CRUD commands: `add`, `show`, `mod`, `start`, `done`, `del`, `list`,
  and `clear`.
- Project commands: `add_project`, `mod_project`, `del_project`,
  `list_projects`, and `show_project`.
- Task metadata: numeric ID, name, description, priority, due date/time,
  notification bits, recurrence, status, category, and project.
- Project metadata that tasks can inherit.
- JSON persistence in `tasks.json`.
- Human-readable colored table output.
- Sorting by priority and due date.

## Remove

These TDL features do not belong in Dispatch:

- Due dates and date/time storage.
- Recurrence and startup recurrence updates.
- Notification metadata and notify/date normalization.
- Categories as currently implemented.
- `clear` behavior for completed personal todos.
- TDL project inheritance semantics.
- Default task/project names such as `Task N` and `Project N`.
- Personal todo lifecycle as the only model: `todo`, `ongoing`, `done`.
- Due-date sorting as the default ordering rule.

## Keep Or Adapt

These features remain useful, but need Dispatch-specific semantics:

- Delete tasks, with safeguards when other tasks depend on the target.
- List and show commands.
- Human-readable table output.
- JSON persistence, renamed from `tasks.json` to `dispatch.json`.
- Priority only if redesigned as optional workflow metadata, not as the main
  ordering model.
- Filtering if it stays simple and does not complicate the workflow model.

## Add

Dispatch should add these core features:

- Meaningful task IDs generated from the task group/topic, such as `DEV-01`.
- Boards as the top-level workspace/file concept.
- Groups as the main way to organize tasks by topic or lane.
- Explicit dependency tracking with a `depends_on` list of task IDs.
- Derived blocked state: a task is blocked when any dependency is not done.
- A review gate field, likely named `requires_review`, that stops agents after
  completing a step until the user has reviewed it.
- Assignment tracking with `assigned_to`.
- Ownership history with `started_by`, `completed_by`, `started_at`,
  `completed_at`, and `updated_at`.
- Task notes or history entries so agents can append progress without
  overwriting the task description.
- Legal workflow transitions enforced by the CLI.
- Dependency cycle detection.
- Claim/lock behavior so multiple agents do not casually start the same task.
- Sequence-aware display that shows groups and dependency chains.
- Machine-readable output, likely JSON, for agents.
- A normalize or repair command that assigns missing IDs and recomputes derived
  state.

## Version One Schema

Dispatch stores one board in `dispatch.json`. The v1 storage shape is:

```json
{
  "version": 1,
  "board": {
    "name": "Dispatch",
    "groups": [],
    "tasks": []
  }
}
```

Groups organize tasks:

```json
{
  "id": "DE",
  "name": "Development",
  "prefix": "DE"
}
```

Tasks carry workflow state and ownership:

```json
{
  "id": "DE-01",
  "title": "Implement board model",
  "description": "Build the core Dispatch data model.",
  "group": "DE",
  "state": "ready",
  "depends_on": ["RE-05"],
  "requires_review": true,
  "assigned_to": null,
  "started_by": null,
  "completed_by": null,
  "created_at": null,
  "started_at": null,
  "completed_at": null,
  "updated_at": null,
  "history": []
}
```

`depends_on` stores task IDs only. Reverse relationships such as "blocks" are
computed for display.

`blocked` is a derived state. A task is blocked when any task listed in
`depends_on` is not `done`.

## Command Vocabulary

The v1 CLI should use Dispatch workflow language:

- `init`: create `dispatch.json` if it does not exist.
- `group add <name>`: create a group and prefix.
- `task add <group> <title>`: create a task.
- `task edit <id>`: update task title or description.
- `task delete <id>`: delete a task, guarded when it has dependents.
- `dep add <from-id> <to-id>`: make one task block another.
- `dep remove <from-id> <to-id>`: remove a dependency.
- `ready <id>`: mark a proposed task ready for work.
- `start <id> --actor <name>`: assign and move a ready task to doing.
- `pause <id> --actor <name>`: return a doing task to ready.
- `finish <id> --actor <name>`: move doing to review or done depending on
  `requires_review`.
- `review <id> --actor <name>`: accept a review task as done.
- `list`: show tasks by group and workflow order.
- `ready`: list ready work.
- `blocked`: list blocked work with blockers.
- `show <id>`: show one task with dependencies, dependents, ownership, and
  history.
- `normalize`: repair IDs and recompute derived states.

Commands that return data for agents should support a JSON output mode.

## Proposed Lifecycle

The exact names can still change, but Dispatch needs more states than TDL:

- `proposed`: suggested work awaiting human approval.
- `ready`: approved work with all dependencies satisfied.
- `blocked`: derived state for work with unmet dependencies.
- `doing`: actively assigned work.
- `review`: finished by an agent or user, awaiting review when required.
- `done`: completed and accepted.
- `paused`: started work that has been intentionally returned to the queue.

## Display And Ordering

The default view should not be priority-first. A useful order is:

1. Board.
2. Group.
3. Dependency depth or sequence order.
4. State.
5. Task ID.

Dispatch should provide views for:

- All tasks by group.
- Ready tasks.
- Blocked tasks and their blockers.
- Review tasks.
- Tasks assigned to a specific agent or user.
- Sequence/tree output that makes dependencies visible.

## Open Questions

- Should the top-level command call these containers boards, projects, or
  workspaces?
- Should groups fully replace TDL projects?
- Is `requires_review` the right name for the user-check gate?
- What exact identifier format should be used for agents and users?
- Should delete require confirmation or a force flag when a task has
  dependents?
- Should priority remain in version one, or wait until the workflow model is
  stable?
- What should the terminal sequence view look like?
