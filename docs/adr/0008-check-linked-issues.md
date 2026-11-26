# 0008: Check linked GitHub issues and close on review

- **Status:** Accepted
- **Date:** 2026-11-25
- **Refined by:** [ADR 0013](0013-shared-issue-links.md), which treats an
  issue linked by several tasks as one piece of work: `duplicate_issue` only
  across groups, `issue_still_open` only once every linking task is done, and
  `review --close-issue` closes only for the last open linking task.

## Context

GitHub issue #6 builds on [ADR 0001](0001-github-issue-links.md) and
[ADR 0007](0007-task-from-github-issue.md): once tasks link GitHub issues,
board state and GitHub issue state can drift apart. A task may be completed
while its issue is left open, or an issue may be closed while its task is still
open on the board. Duplicate issue links across multiple tasks can also occur
(already checked offline by `status`).

Because `status` runs frequently and in offline or headless environments,
issue checks that touch the network must be opt-in (`dispatch status
--check-issues`), must integrate with the version 1 JSON contract, and must
fail gracefully when `gh` is unavailable or unauthenticated. Issue #6 also
includes an optional `dispatch review --close-issue` flag to close a task's
linked issue with a comment when the task is accepted.

## Decision

**Opt-in network checks on `status`.** Plain `dispatch status` and `dispatch
status --json` stay completely offline and never invoke `gh`; they continue to
report `duplicate_issue` locally. Passing `--check-issues` opts into querying
GitHub via `gh issue view <N> --repo <owner/repo> --json state` for each unique
linked issue on the board. Issue refs are deduplicated case-insensitively on
`owner/repo`, matching GitHub, so duplicate links trigger only one `gh` call.
If no task on the board links an issue, `gh` is not invoked.

**Drift rules.** Task state uses the same presentation state shown by `status`
and `list`: `done` vs non-`done` (`proposed`, `ready`, `blocked`, `doing`,
`review`, `paused`).

- A `done` task whose linked issue is `OPEN` emits `issue_still_open`.
- A non-`done` task whose linked issue is `CLOSED` emits
  `issue_already_closed`.
- Multiple tasks linking the same issue continue to emit `duplicate_issue`
  offline.

**Reuse the shared `dispatch_gh` helper and error codes.** Both
`status --check-issues` and `review --close-issue` build on the shared
`dispatch_gh` module introduced in [ADR 0007](0007-task-from-github-issue.md)
(which runs `gh` via `dispatch_exec_capture_streams` so stdout JSON and stderr
diagnostics never interleave or leak into JSON mode). The shared module gains
two backward-compatible entry points — `dispatch_gh_issue_state` and
`dispatch_gh_issue_close` — without altering `dispatch_gh_issue_view` or any
existing status/error codes. Failures reuse `dispatch_gh_error_code` directly
(`gh_missing`, `gh_not_authenticated`, `issue_not_found`, `invalid_issue`,
`gh_failed`) so every GitHub-enabled command reports the same machine-readable
codes.

**Non-fatal `gh` handling on `status`.** `status` is a read command, and read
commands in schema version 1 report errors only as plain text on stderr rather
than JSON error documents. Failing hard when `gh` is missing or unauthenticated
would hide the rest of the board overview and leave JSON consumers without
structured diagnostics. Instead, `--check-issues` treats `gh` problems as
non-fatal warnings and exits 0:

- `gh` not installed or not on `PATH` emits one board-level `gh_missing`
  warning (`task_id: null`, `agent: null`) and stops checking.
- `gh` not authenticated (exit code `4` or auth error output) emits one
  board-level `gh_not_authenticated` warning (`task_id: null`, `agent: null`)
  and stops querying further issues.
- A lookup error for an individual issue emits a per-task warning using the
  shared `dispatch_gh_error_code` (`issue_not_found` for missing issues/repos,
  or `gh_failed` for other errors / unreadable JSON / unexpected states) for
  each task linking that issue (reusing the cached result) and continues
  checking the rest of the board.

**JSON contract changes.** All changes are additive so `schema_version` stays
`1`:

- `status --json` adds `query.check_issues` (boolean) so consumers can tell
  whether live issue checks ran.
- Issue-check warnings appear in `warnings` after the offline warnings, keeping
  the base `{code, message, task_id, agent}` fields. Per-task issue warnings
  (`issue_still_open`, `issue_already_closed`, `issue_not_found`, `gh_failed`)
  also carry `issue` (`owner/repo#N`), because `status`'s `tasks` array only
  contains `ready` and `review` tasks and omits `done`, `doing`, `blocked`, and
  `proposed` tasks.

**`review --close-issue`.** `dispatch review <id>... [--actor <name>]
[--close-issue]` validates and saves the accepted task(s) first, releases the
board lock before doing network I/O, and then closes each unique linked issue
via `dispatch_gh_issue_close` (`gh issue close <N> --repo <owner/repo>
--comment <text>`) with a comment naming the task ID, title, reviewing actor,
and any recorded commits. Each successful close appends an `issue_close` record
to `dispatch.log`. Because the board transition is already committed, close
failures are non-fatal: text mode prints `Warning:` to stderr while `--json`
mode keeps stderr empty and adds `closed_issues` and `issue_close_errors`
arrays to `result` (omitted when `--close-issue` is not passed), with each
error carrying the shared `dispatch_gh_error_code` (`gh_missing`,
`gh_not_authenticated`, `issue_not_found`, or `gh_failed`).

## Consequences

- `status` stays fast and offline by default; `--check-issues` is strictly
  opt-in.
- All GitHub CLI commands (`task add --from-issue`, `status --check-issues`,
  `review --close-issue`) share one `gh` subprocess layer and one set of error
  codes.
- Missing or unauthenticated `gh` degrades gracefully to a single machine-
  readable warning without breaking `status` or losing offline diagnostics.
- Both `status --check-issues` and `review --close-issue` are fully testable
  offline with a stub `gh` placed on `PATH`.

### Follow-ups

- Boards with many linked issues perform one `gh issue view` process per unique
  issue; batching via GraphQL could reduce latency if boards grow large.
- `review --close-issue` does not check whether the issue is already closed
  before calling `gh issue close`.
- Interactive TUI surfaces for `--check-issues` and `--close-issue` remain out
  of scope.
