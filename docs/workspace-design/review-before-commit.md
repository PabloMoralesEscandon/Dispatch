# Review Before Commit Workflow

Task: WD-09

## Decision

Dispatch should support a review-before-commit agent workflow:

1. agent starts a ready task;
2. agent edits files and runs checks;
3. agent presents the uncommitted diff for user review;
4. user accepts or requests changes;
5. only after acceptance, the agent commits the task result;
6. the task moves to `done` and dependents may become ready.

The current `finish -> review -> done` state model already separates "agent says
work is ready" from "user accepts work." The missing piece is aligning commit
timing with that review gate.

## Target Protocol

For review-required tasks:

```text
ready -> doing -> review -> done
```

Agent flow:

```bash
dispatch start <task-id> --actor <agent-id>
# edit files
# run checks
# present diff
```

After user acceptance:

```bash
git add <task files>
git commit -m "<task-id> <task title>"
dispatch finish <task-id> --actor <agent-id>
dispatch review <task-id> --actor user
```

The exact command order may be adjusted during implementation, but the invariant
is that review-required work is not committed until the user accepts the diff.

## Multiple In-Flight Tasks

Use one git worktree and branch per task when more than one task is in flight:

```bash
git worktree add ../Dispatch-agent-codex-WD-09 -b agent/codex/WD-09
```

Each task branch can hold uncommitted review changes independently. After the
user accepts a task, the agent commits that task branch. The integrator merges
accepted task branches one at a time into the main branch.

Do not combine several reviewed tasks into one commit. Dispatch keeps one task
commit per completed task.

## Dependent Tasks

A dependent task may start only when its dependency is accepted and its commit is
available in the branch or worktree used for the dependent task.

For review-required tasks, `review` is the acceptance gate. For no-review tasks,
`finish` may move directly to `done`, so the commit can happen immediately after
checks pass.

## Documentation Follow-Up

The agent instructions should be revised to replace "finish then commit" with
the review-before-commit protocol. Until that change is implemented, agents must
follow the active repository instructions for commit discipline.
