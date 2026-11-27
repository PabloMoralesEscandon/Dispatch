# Dispatch warnings: what they mean and what to do

Warnings come from `dispatch status`, `dispatch status --check-issues`,
`dispatch doctor` and the `warnings` array of any `--json` result. Match on the
code (`code` in `status` and mutation results, `kind` in `doctor --json`),
never on the message text. Each row says what to do; "report" means tell the
person and let them decide. Fixes marked **ask first** change or delete things
the person may want to keep: show what would change and wait for their
go-ahead.

## Board warnings (`status`, also checked by `doctor`)

| Code | Meaning | What to do |
| --- | --- | --- |
| `missing_commits` | A `done` or `review` task has no recorded commit. | If you made the commit, record it: `dispatch commit add <TASK-ID> <sha> --actor <your-agent-name>`. Otherwise report it; do not guess a sha. |
| `assignment_state_mismatch` | A task is assigned but not `doing` or `review`. | `dispatch normalize`. |
| `missing_current_task` | An agent session points at a task that does not exist. | `dispatch agent session <name> --clear-current-task` for your own agent; report it for others. |
| `duplicate_issue` | Tasks in **different groups** link the same GitHub issue. Tasks of one group sharing an issue are fine and give no warning. | Report it: either one task is linked to the wrong issue (`dispatch task edit <TASK-ID> --issue <ref>` or `--clear-issue`) or the tasks belong in one group (`dispatch task move`). |

## Linked issue checks (`status --check-issues`)

| Code | Meaning | What to do |
| --- | --- | --- |
| `issue_still_open` | Every task linking the issue is `done`, but the issue is still open on GitHub. | Offer to close it (`gh issue close`, or `review --close-issue` next time). |
| `issue_already_closed` | A task that is not `done` links an issue already closed on GitHub. | Report it: the task may be obsolete, or the issue was closed too early. |
| `issue_not_found` | The linked issue does not exist or cannot be seen. | Report it; fix the link with `dispatch task edit <TASK-ID> --issue <ref>` or `--clear-issue` if the person agrees. |
| `gh_missing`, `gh_not_authenticated` | `gh` is not installed or not logged in, so no issue was checked. | Report it; the person runs `gh auth login`. The rest of `status` is still valid. |
| `gh_failed` | `gh` failed for one issue (network, rate limit). | Retry later; report it if it persists. |

## Mutation warnings (`warnings` in a `--json` result)

The command succeeded; the warning is about a side effect.

| Code | Meaning | What to do |
| --- | --- | --- |
| `issue_left_open` | `review --close-issue` left a shared issue open because other tasks linking it are not done yet. | Nothing; it closes when the last of them is reviewed with `--close-issue`. Mention which tasks remain. |
| `issue_close_errors` (in `result`, not `warnings`) | `review --close-issue` could not close an issue; each entry has a `gh` error code. The review itself was recorded. | Report it and offer to close the issue by hand. |
| `log_append_failed` | The board was saved but the `dispatch.log` record was not written. | Report it (disk full, permissions); do not retry the command. |
| `scaffold_failed` | `init` could not create the `.dispatch` directories. | Report it; the board itself was created. |
| `repo_not_git` | `repo set` points at a directory that is not a Git repository. | Report it; `dispatch repo set <path>` with the right path. |
| `skill_modified`, `skill_unmanaged`, `skill_newer` | `skills install`/`update` skipped a copy the person edited, did not install, or that a newer Dispatch installed. | Leave it. Use `--force` only when the person asks. |
| `home_unset` | `HOME` is not set, so global skills were not checked. | Report it. |
| `malformed_log_lines`, `unsupported_log_version` | `dispatch log` skipped unreadable lines. | Report it; never edit `dispatch.log` yourself. |

## Doctor checks (`doctor`)

`doctor` prints a `fix:` hint under each warning. `[info]` lines are notes, not
problems.

| Kind | Meaning | What to do |
| --- | --- | --- |
| `board_load_failed` | `dispatch.json` cannot be loaded. | Report the error. Never open or edit the file; the person resolves it, then `dispatch normalize`. |
| `repo_not_git` | The board's repository is not a Git repository. | Report it; `dispatch repo set <path>` if it moved. Never run `dispatch init` on an existing board. |
| `dispatch_not_on_path`, `workflow_entry_missing`, `workflow_entry_unexpected_type` | The `dispatch` binary is not on `PATH`, or the workflow directory's `dispatch` link is missing or not a symlink. | Report it with the `fix:` hint; it is the person's setup. |
| `completion_missing`, `completion_home_unset` | Shell completion is not installed. | Optional; `dispatch completion install` with `bash`, `zsh` or `fish` if the person wants it. |
| `skills_missing` | A Dispatch skill is installed for no runner. | `dispatch skills install` (and `--global` for sessions outside the workflow directory). |
| `skills_outdated` | An installed copy is older than the binary. | `dispatch skills update` (or `--global`, as the hint says). |
| `skills_project_only` | Skills are only in the workflow directory, but some workspaces live outside it, so agents there will not find them. | `dispatch skills install --global`. |
| `instructions_redundant` (info) | `AGENTS.md`/`CLAUDE.md` only holds the old Dispatch template; the skills replace it. | Offer to remove the file once the skills are installed. **Ask first**; never edit or remove it on your own. |
| `agent_prompt_missing`, `agent_prompt_outdated`, `agent_run_script_not_executable` | An agent's generated prompt or run script is missing or stale. | `dispatch normalize`. |
| `missing_current_task` | As in `status`. | As above. |
| `workspace_path_missing` | A workspace's directory is gone. | `dispatch workspace prune --stale --dry-run`, then apply; or `dispatch workspace remove <id>`. |
| `workspace_path_moved` | The workflow directory moved and the workspace path is stale. | `dispatch workspace repair <id>`. |
| `workspace_not_a_worktree`, `workspace_branch_mismatch` | An adopted worktree is no longer a worktree of the board's repository, or has another branch checked out. | Report it; follow the `fix:` hint once the person confirms which side is right. |
| `orphaned_workspace` | A workspace record has no live task. | `dispatch workspace remove <id> --force`. **Ask first.** |
| `dep_self`, `dep_missing_reference`, `dep_duplicate` | A broken dependency: on itself, on a missing task, or listed twice. | `dispatch dep remove <dependency-id> <dependent-id> --actor <your-agent-name>`. |
| `assignment_state_mismatch`, `start_provenance_mismatch` | Assignment data disagrees with task state or history. | `dispatch normalize`. |
