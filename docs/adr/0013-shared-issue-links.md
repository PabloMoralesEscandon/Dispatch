# 0013: Several tasks may share a GitHub issue

- **Status:** Accepted
- **Date:** 2026-11-26
- **Refines:** [ADR 0001](0001-github-issue-links.md),
  [ADR 0008](0008-check-linked-issues.md)

## Context

[ADR 0001](0001-github-issue-links.md) lets a task link at most one GitHub
issue, and `status` warned `duplicate_issue` for every task after the first
that linked an issue already linked earlier. [ADR 0008](0008-check-linked-issues.md)
added the live checks `status --check-issues` (`issue_still_open`,
`issue_already_closed`) and `review --close-issue`, all reasoning about one
task at a time.

The planning rules (the `dispatch-plan` skill) ask agents to break work into
granular, reviewable tasks, and a GitHub issue usually describes a feature,
not one step. Linking every task of that feature to its issue is the natural
result, and it made `status` noisy: a real board had 23 of its 34 tasks
sharing an issue with a sibling, so it showed 23 permanent `duplicate_issue`
warnings that hid any real one (issue #29). The live checks had the same
assumption: `issue_still_open` fired as soon as the first of several tasks
was done, and `review --close-issue` closed the issue when the first task was
accepted while its siblings were still open.

## Decision

Treat an issue linked by several tasks as **one piece of work split into
tasks**. Two tasks share an issue when their canonical refs match,
case-insensitively as before.

**`duplicate_issue` only across groups.** The planning rules put one feature
in one group, so tasks of the same group sharing an issue are expected and get
no warning. When the tasks linking an issue span more than one group the
warning stays, with the same code, emitted for each task after the first in
board order. Its message now names every other task linking the issue:
`ST-04 links acme/widgets#22, also linked to OT-01, ST-05`. No quieter `info`
level was added: the same-group case needs no diagnostic, and `show` already
makes the split visible.

**`issue_still_open` when every linking task is done.** With
`--check-issues`, a `done` task whose issue is still `OPEN` warns only when
every task linking that issue is `done`; until then an open issue is expected.
The warning is still emitted per task, so once the last task is done each
linking task reports it.

**`issue_already_closed` unchanged.** A closed issue with any task still open
is drift for that task, shared or not.

**`review --close-issue` closes on the last task.** The review itself always
succeeds as before. For each reviewed task with an issue, Dispatch checks the
board after the review: if another task linking the same issue is not `done`,
the issue is left open and `gh issue close` is not called. Text mode prints
`Left <issue> open for <task>: still linked by <ids>`; `--json` adds a
non-fatal `issue_left_open` warning to the document's `warnings` array with
`task_id`, `issue`, and a message listing the sibling IDs. It is not an entry
in `result.issue_close_errors`, since nothing failed. A batch review that
accepts every remaining sibling closes the issue once, as before, because the
whole batch is applied before any issue is closed.

**`show` lists the siblings; `list` does not change.** Text `show` appends
`(also <ids>)` to the `Issue:` line. `show --json` adds `issue_siblings` (array
of task IDs in board order, empty when none or when the task links no issue)
to the task object. Other commands' task objects are unchanged, so `list`
output and its JSON stay as they are.

## Consequences

- Boards that link one issue to the tasks of one feature have no permanent
  warnings; `duplicate_issue` now points at a likely mistake (one issue split
  across unrelated groups).
- `issue_still_open` and `review --close-issue` follow the issue's real
  lifecycle: the issue stays open until its last task is done.
- All JSON changes are additive (`issue_left_open`, `issue_siblings`,
  `issue` on mutation warnings), so `schema_version` stays `1`. The meaning of
  `duplicate_issue` and `issue_still_open` narrows; consumers that counted
  them see fewer warnings.
- Tasks in different groups that legitimately share an issue still warn; move
  them into one group or link the issue to one of them.
