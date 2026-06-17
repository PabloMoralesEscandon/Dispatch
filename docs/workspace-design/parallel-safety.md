# Parallel Safety And Locking

Task: WD-06

## Decision

Dispatch should support parallel agents through two layers:

1. a process-level lock around every board mutation;
2. task and workspace ownership checks in the board state.

Git worktrees provide filesystem isolation. Dispatch ownership prevents two
actors from claiming the same task or workspace.

## Board Lock

Every command that writes board state must acquire an exclusive lock next to the
store file:

```text
dispatch.json.lock
```

The lock is held only while loading, validating, mutating, and saving
`dispatch.json`. It is not held while running long git commands. For workspace
creation, Dispatch should:

1. acquire lock and reserve the workspace record as `creating`;
2. release lock;
3. run git worktree operations;
4. reacquire lock and mark workspace `active`, or remove the reservation on
   failure.

This avoids long lock holds while still making concurrent creates collide on the
reservation.

## Ownership Invariants

The board must reject mutations that violate these invariants:

- one task can have at most one active workspace;
- one workspace path can belong to at most one task;
- one branch can belong to at most one active workspace;
- a task with `assigned_to` cannot be started by another actor;
- a task workspace actor must match the task assignment actor once assigned.

## Concurrent Command Behavior

If another process holds the board lock, Dispatch waits briefly and retries. If
the timeout expires, it exits non-zero and prints:

```text
Dispatch board is locked by another process; retry shortly.
```

The first implementation can use a short fixed timeout. A later version may add
`--wait` and `--no-wait`.

## Stale Reservations

Workspace records in `creating` state include `created_at` and the creating
actor. If creation crashes, later commands can report the stale reservation.

`workspace prune --stale` may remove `creating` records older than a conservative
timeout only if no matching git worktree exists.

## Agent Guidance

Agents working in parallel must still use separate git worktrees. Dispatch does
not make one checkout safe for multiple agents. The lock only protects Dispatch
board state; it does not serialize edits inside a product repository.
