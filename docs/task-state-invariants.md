# Task State And Assignment Invariants

This contract defines how task assignment fields relate to task state, what
state transitions must do to those fields, and how corrupted boards are
recovered. It exists because a task once became an unstartable zombie:
`state=ready` with `assigned_to` still set, after `dispatch ready` was run on
a task that was already `doing`. `start` refuses assigned tasks, and no
command could clear the assignment, so the task was permanently stuck.

## Field Roles

- `assigned_to` — current ownership. Means "an actor is actively responsible
  for this task right now".
- `started_by`, `started_at` — provenance of the current activation. Set when
  the task is started; meaningful only while that activation is in flight
  (doing, paused, review).
- `completed_by`, `completed_at` — provenance of completion. Set by `finish`
  and `review`; never cleared by returning a done task's dependents to work.

## The Invariant

A task's assignment fields must agree with its state:

| State            | `assigned_to`        | `started_by` / `started_at` |
| ---------------- | -------------------- | --------------------------- |
| proposed         | empty                | empty                       |
| ready            | empty                | empty                       |
| blocked          | empty                | empty                       |
| doing            | set (the actor)      | set                         |
| paused           | set (the actor)      | set                         |
| review           | empty (finish clears)| set                         |
| done             | empty (finish clears)| set                         |

In short: **a task that is available to start (proposed, ready, blocked) must
carry no assignment and no in-flight start provenance.** Assignment exists
only between `start` and `finish`.

## Transition Rules

1. `dispatch ready <id>` on a task that is currently `doing`, `paused`, or
   `review` is a deliberate revert. It must restore the full invariant:
   clear `assigned_to`, `started_by`, and `started_at`, and record the revert
   in history. The result is a clean, immediately startable ready task.
2. `dispatch start <id> --actor <a>` requires state `ready` (effective) and
   an empty `assigned_to`. When it refuses, it must say exactly why:
   unknown task, wrong state (naming the state), or already assigned
   (naming the assignee and pointing at `dispatch unassign`).
3. `dispatch finish` keeps clearing `assigned_to` when moving to
   `review`/`done`, and preserves `started_by` as provenance.

## Escape Hatch: `dispatch unassign`

```text
dispatch unassign <TASK-ID> --actor <name>
```

Clears a stuck or abandoned assignment without editing storage:

- Allowed when `assigned_to` is set and state is `proposed`, `ready`,
  `blocked`, `doing`, or `paused`. A `doing`/`paused` task returns to
  `ready` (or `blocked` if dependencies are unmet, via normalization).
- Refused for `review` and `done` tasks: those have completion provenance,
  and unassigning them would silently discard a finished result. Their
  `assigned_to` is already empty under the invariant; if storage disagrees,
  that is a repair case for `normalize`, not `unassign`.
- Clears `assigned_to`, `started_by`, `started_at`; appends an `unassigned`
  history entry with the acting actor.

## Detection And Repair

- `dispatch doctor` flags every task whose fields violate the table above
  (for example `ready` with `assigned_to` set, or `proposed` with
  `started_by` set) as a warning naming the task and the violation.
- `dispatch normalize` repairs those violations by clearing the offending
  fields, so healing an already-corrupted board is a single
  `dispatch normalize`. Repairs are limited to invariant restoration;
  normalize never invents state.

## Recovery Procedure For Operators

For a task stuck "ready but assigned" (the zombie):

```bash
dispatch normalize        # repairs the invariant violation in place
dispatch show <TASK-ID>   # verify: ready, unassigned
dispatch start <TASK-ID> --actor <owner>
```

`dispatch unassign` covers the live variant (an agent abandoned a `doing`
task) where waiting for a full normalize pass is not desired. Task history
and recorded commits are preserved in both paths; recovery never deletes or
recreates tasks.

## Worked Example: The RF-01 Recovery

RF-01 was the original zombie: `dispatch ready RF-01` was run while the task
was `doing`, leaving it `ready` but still assigned, and `start` refused it
with only a generic message. After the fixes above landed, the recovery was:

```text
$ dispatch doctor
[warn] task RF-01 is ready but still assigned to claude-problem-solver
       fix: repair it with dispatch normalize
$ dispatch unassign RF-01 --actor claude-problem-solver
Unassigned RF-01 from claude-problem-solver (ready)
$ dispatch start RF-01 --actor claude-problem-solver
Started RF-01
```

The task kept its full history (including an `unassigned` entry recording
the repair) and its previously recorded commit, and completed normally.
