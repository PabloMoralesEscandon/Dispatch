# Task Assignment Integration

Task: WD-05

## Decision

Workspace creation is independent from task assignment, but both operations
must agree on actor and task ownership.

`dispatch workspace create` prepares an isolated checkout. `dispatch start`
claims the task. Neither command silently performs the other operation.

Recommended sequence:

```bash
dispatch workspace create WD-05 --actor codex
dispatch start WD-05 --actor codex
```

## Ownership Rules

Workspace creation requires:

- task state is `ready`;
- task has no `assigned_to`;
- no workspace is already recorded for the task;
- actor label is valid.

Task start requires:

- task state is `ready`;
- task has no `assigned_to`;
- if a workspace exists for the task, the start actor matches the workspace
  actor.

If a task has a workspace for `codex`, then `dispatch start WD-05 --actor other`
fails and prints the recorded workspace owner.

## Recorded State

The board should store workspace metadata with the task or in a dedicated
workspace list:

```text
task_id: WD-05
actor: codex
path: /abs/path/Dispatch-agent-codex-WD-05
branch: agent/codex/WD-05
created_at: <timestamp>
state: active
```

`show`, `list`, and `workspace show` should surface the workspace path and
branch when present.

## Why Not Auto-Create On Start

`start` should remain a lightweight state transition. Creating a git worktree
can fail for filesystem, branch, or git-repository reasons. Keeping it explicit
avoids a partially assigned task when checkout creation fails.

## Recovery

If workspace creation succeeds but `start` fails, the workspace remains
recorded and can be inspected or removed:

```bash
dispatch workspace show WD-05
dispatch workspace remove WD-05
```

If `start` succeeds without a workspace, Dispatch still allows the task. This
supports single-agent or documentation-only workflows where a separate worktree
is unnecessary.
