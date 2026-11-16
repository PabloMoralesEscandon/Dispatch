# 0001: Link a GitHub issue to a task

- **Status:** Accepted
- **Date:** 2026-11-16

## Context

GitHub issue #1 asks that every Dispatch task be traceable to a GitHub issue,
so planned work shows up where it is tracked and the Orca plugin can open a
task's worktree with Orca's `--issue` link. The issue sets the overall shape:
an optional per-task field, `task add --issue` and `task edit --issue` /
`--clear-issue`, `issue_link` / `issue_unlink` records, output in `show`,
`list`, the TUI inspector, and JSON, plus shell completion. It leaves several
details open, and existing boards must keep loading unchanged.

## Decision

**Storage.** A task links at most one issue, stored as the string
`owner/repo#number` under an optional `issue` key. The key is written only
when set, so boards without linked issues keep their exact shape, including
after `normalize`. A missing key and `null` both mean "no issue". A stored
value that is not a canonical ref fails board load, like other invalid task
fields.

**Accepted input.** `--issue` accepts `123`, `#123`, `owner/repo#123`, and
`https://github.com/owner/repo/issues/123`. The URL form tolerates `http`,
`www.`, a trailing slash, and a query or fragment, so a copied comment link
works. Owner and repo must be valid ASCII GitHub names, and numbers must be
positive with no leading zero. Pull request URLs, non-GitHub hosts, and
anything else are rejected before the board changes, and no log record is
written.

**Bare numbers.** `123` and `#123` resolve against the board repository's
`origin` remote, read with the local `git remote get-url origin`. Only
`github.com` remotes resolve (HTTPS, SSH, scp-like, and `git://` forms). With
no origin, or a non-GitHub origin (including GitHub Enterprise or SSH host
aliases), the command fails and points at the `owner/repo#N` form.

**No network.** Linking never contacts GitHub and never runs `gh`, so Dispatch
does not check that the issue exists. This keeps linking fast, offline, and
free of credentials.

**History and log.** Linking records `issue_link` and clearing records
`issue_unlink`, in task history (ref as the note) and in `dispatch.log` (ref
in `context.issue`). Replacing an issue records an unlink of the old ref and
then a link of the new one, so every link pairs with its unlink.

**No-op edits.** `task edit --issue` with the already-linked ref, and
`--clear-issue` on a task with no issue, succeed with a message and write
nothing: no history, no log record, no save.

**Output.** `show` prints `Issue: owner/repo#N (url)` or `Issue: -`. `list`
adds `issue:#N` to the task line even when the issue lives in another repo;
`show` and JSON carry the full ref. JSON task objects gain a nullable `issue`
object `{repo, number, url}`. The change is additive, so `schema_version`
stays 1.

**Duplicates.** Several tasks may link the same issue. `status` (and
`status --json`, code `duplicate_issue`) warns for each task that links the
same issue as an earlier task in board order, comparing repos
case-insensitively. The warning includes done tasks and is not reported by
`doctor`.

**TUI.** The task inspector shows an Issue field. `o` opens the issue URL with
`open` on macOS or `xdg-open` elsewhere, from the task inspector only; the
board view does not bind it. The TUI task creation and edit forms cannot set
an issue yet; linking happens through the CLI.

**Agent templates.** The `AGENTS.md` and `CLAUDE.md` templates that
`dispatch init` writes list `--issue` on `task add` and a
`task edit --issue|--clear-issue` line, with a sentence telling agents to link
the GitHub issue when work is tracked in one.

## Consequences

- Old boards load and normalize byte-for-byte unchanged with respect to
  issues, and JSON consumers that ignore unknown fields keep working.
- A linked issue may not exist or may be closed; consumers that need that
  certainty must check GitHub themselves.
- Boards whose repo is hosted outside `github.com` must use the full
  `owner/repo#N` or URL form.
- `list` alone cannot tell a same-repo issue from a cross-repo one with the
  same number; use `show` or JSON for the full ref.
- The duplicate warning can fire for intentional follow-up tasks on the same
  issue, including completed ones; it is advisory only.
- Setting an issue from the TUI forms, and a `doctor` check for duplicates,
  remain possible follow-ups.
- Boards created before this change keep their existing `AGENTS.md` /
  `CLAUDE.md`, since `init` does not overwrite them; only new boards get the
  `--issue` guidance.
