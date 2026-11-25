# 0006: Create a task from a GitHub issue

- **Status:** Accepted
- **Date:** 2026-11-25

## Context

GitHub issue #5 builds on ADR 0001 (linking a GitHub issue to a task) and asks
for `dispatch task add <group> --from-issue <ref>`, which fetches an issue's
title and body through `gh issue view --json title,body,url`, creates a task
from them, and links the issue in one step.

Unlike `--issue` (which stays strictly offline per ADR 0001), `--from-issue`
contacts GitHub via the `gh` CLI. That introduces several design choices: how
`--from-issue` interacts with an explicit `<title>`, `--description`, or
`--issue`; how bare numbers and pull requests are handled; how board locking
behaves across a network call; how `gh` failures map to human messages and
ADR 0002 JSON error codes; and how the `gh` subprocess wrapper should be
structured so future GitHub-backed commands (such as issue #6's
`status --check-issues`) can share it.

## Decision

**Two non-overlapping `task add` forms (reject conflicts).** `task add` now has
two explicit forms:

- `dispatch task add <group> <title> [--description <text>] [--actor <name>] [--no-review] [--issue <ref>]`
- `dispatch task add <group> --from-issue <ref> [--actor <name>] [--no-review]`

Combining `--from-issue` with an explicit `<title>`, `--description`, or
`--issue` is rejected with a `usage` error before contacting `gh` or modifying
the board. Repeating `--from-issue` is also a `usage` error. This avoids
ambiguity (such as whether `--description` would replace or append to the issue
body, or whether someone meant offline `--issue`), while keeping `--actor`,
`--no-review`, and `--json` available on both forms. Callers that want a custom
title or description can use `task add <group> <title> --issue <ref>` or follow
up with `task edit`.

**Resolve refs locally before invoking `gh`.** `--from-issue` accepts the same
four ref forms as `--issue` (`123`, `#123`, `owner/repo#123`, and GitHub issue
URLs). Malformed refs fail immediately with `invalid_issue`. Bare numbers
resolve against the board repository's GitHub `origin` remote via local `git`,
failing with `issue_unresolvable` when none exists. Dispatch then calls:

```text
gh issue view <number> --repo <owner>/<repo> --json title,body,url
```

Passing `--repo` explicitly makes the call independent of the workflow
directory's working tree and works identically for cross-repo references.

**Canonical link from `url` and pull-request rejection.** On GitHub, issues and
pull requests share one numbering space, and `gh issue view` succeeds on a PR
number, returning a `/pull/N` URL. Dispatch parses the returned `url` through
`dispatch_issue_parse`: a valid GitHub issue URL provides the canonical
`owner/repo#number` to link (picking up GitHub's canonical casing and following
transferred issues), while a `/pull/` URL is rejected with `invalid_issue` so
pull requests are never linked as issues.

**Title and body mapping.** The fetched title has surrounding whitespace
trimmed and goes through the same `task add` checks (non-empty, must not start
with a Dispatch ID), failing with `invalid_title` if rejected. The fetched body
normalizes `\r\n` and `\r` line endings from GitHub web forms to `\n`, and a
`null` or missing body becomes `""`.

**Unlock across the network call.** `dispatch.json.lock` has a 1-second timeout
meant for local disk I/O. For `--from-issue`, Dispatch takes a short read load
to verify the group exists and resolve any bare number, releases the lock, runs
`gh`, and only then acquires the write lock to create, link, and save the task.
In `--json` mode the pre-mutation snapshot is taken on the locked load so
`newly_ready` reflects only this command's change.

**Reusable `gh` helper and dual-stream capture.** `dispatch_exec` gains
`dispatch_exec_capture_streams`, which drains stdout and stderr over two pipes
with `poll(2)`. This prevents pipe-buffer deadlocks, keeps `gh` stderr notices
(such as upgrade banners) from corrupting stdout JSON, and prevents child
stderr from leaking into `--json` error output. `include/dispatch_gh.h` and
`src/dispatch_gh.c` build on it with `dispatch_gh_run`, `dispatch_gh_run_json`,
and `dispatch_gh_issue_view` (always redirecting stdin to `/dev/null`), giving
future commands like `status --check-issues` one shared `gh` layer.

**Error classification and JSON error codes.** Failures map to clear human
messages and four additive version 1 error codes (ADR 0002, `schema_version`
stays 1):

- `gh_missing`: `gh` is not installed or not on `PATH` (or exits 127).
- `gh_not_authenticated`: `gh` exits 4 (its documented auth-required exit code)
  or reports auth failure on stderr (`gh auth login`, `not logged in`,
  `HTTP 401`, `Bad credentials`, etc.).
- `issue_not_found`: `gh` reports that the issue or repository could not be
  resolved (`Could not resolve to an issue`, `Could not resolve to a repository`,
  `HTTP 404`, `not found`).
- `gh_failed`: any other `gh` non-zero exit, signal, or unreadable/incomplete
  JSON output (preserving the first stderr line in the message).

Auth errors are checked before not-found patterns so credential errors are
never misclassified as missing issues.

**Output, history, logs, help, completion, and templates.** On success,
`task add --from-issue` produces the exact same task state, `created` and
`issue_link` history entries, `task/add` and `task/issue_link` log records,
text confirmation lines, and `task_add` JSON result envelope as
`task add --issue`. `dispatch task --help` and `dispatch task add --help` print
both forms; bash, zsh, and fish complete `--from-issue`; and `AGENTS.md` /
`CLAUDE.md` document the option.

## Consequences

- Importing an issue into Dispatch takes one command without copying titles or
  descriptions by hand.
- `--issue` remains fast and completely offline; `--from-issue` is the explicit
  opt-in to `gh` and the network.
- Concurrent Dispatch commands are not blocked by `dispatch.json.lock` while
  `gh` talks to GitHub.
- Future commands that invoke `gh` (such as `status --check-issues` and
  `review --close-issue`) can reuse `dispatch_gh_run` and
  `dispatch_gh_run_json` directly and inherit consistent error classification.
- Four additive JSON error codes are added to the version 1 contract:
  `gh_missing`, `gh_not_authenticated`, `issue_not_found`, and `gh_failed`.

### Follow-ups

- Supporting optional title/description override flags (e.g. `--title`) if
  users frequently import issues whose GitHub titles start with a Dispatch ID
  or carry planning suffixes.
- Creating a task from an issue inside the TUI.
