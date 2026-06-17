# Sequence Workspace Reuse

Task: WD-11

## Decision

The default rule is one workspace per task. Dispatch should also support a
controlled exception: one workspace may cover a direct linear sequence of tasks
when the intermediate tasks do not require review and the sequence ends at the
first review gate.

This supports chains such as:

```text
DE-01 --no-review -> DE-02 --no-review -> DE-03 requires review
```

The agent can work in one branch and produce one commit per task:

```text
DE-01 commit
DE-02 commit
DE-03 commit
```

The user reviews the final branch state at `DE-03`.

## Constraints

Sequence workspace reuse is allowed only when:

- the tasks form a direct linear dependency chain;
- every intermediate task has `requires_review = false`;
- the final task is the first task in the chain that requires review;
- one actor owns the entire sequence;
- no task in the sequence has another active workspace;
- no task in the sequence has unrelated dependents that should start before the
  final review gate.

If the chain branches, joins, or has multiple possible next tasks, Dispatch
should require an explicit task-level workspace for each branch.

## Command Shape

The workspace command may accept an explicit sequence option:

```bash
dispatch workspace create DE-01 --actor codex-server --sequence
```

Dispatch resolves the direct chain from `DE-01` until the first review-required
task and records the workspace as covering that sequence:

```text
workspace: repo-agent-codex-server-DE-01-sequence
branch: agent/codex-server/DE-01-sequence
tasks: DE-01, DE-02, DE-03
review_gate: DE-03
```

## Task Flow

For each intermediate no-review task:

```bash
dispatch start DE-01 --actor codex-server
# edit and test in the sequence workspace
git commit -m "DE-01 ..."
dispatch finish DE-01 --actor codex-server
```

Because the task does not require review, `finish` moves it to `done` and the
next task can become ready.

At the final review gate:

```bash
dispatch start DE-03 --actor codex-server
# edit and test
git commit -m "DE-03 ..."
dispatch finish DE-03 --actor codex-server
```

The sequence stops in `review`. The user reviews the final branch state and then
accepts the gate task.

## Readiness Caveat

Marking intermediate tasks `done` may unblock tasks outside the intended
sequence. Sequence reuse is safe only when those intermediate tasks unblock only
the next task in the same chain, or when the user accepts that other ready tasks
may appear before the final review gate.

Future implementation may add a sequence record that delays external unblocks
until the review gate is accepted. The initial design should document the caveat
and keep sequence use explicit.
