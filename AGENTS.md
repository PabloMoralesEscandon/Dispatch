# Dispatch Agent Rules

Dispatch is a command-line workflow board for humans and agents. It organizes
work into groups, tasks, dependencies, assignment, review gates, and completion
history. This file holds only the rules every session follows; how to plan,
work and review lives in the Dispatch agent skills.

## Critical Rules

1. Use the Dispatch CLI for all workflow state.
2. Never read `dispatch.json` directly.
3. Never edit `dispatch.json` directly.
4. Never infer task state from `dispatch.json`.
5. Pass an explicit `--actor` on every change that accepts one. Use your own
   agent name for your own work; `--actor user` records a decision the user
   made (readying or accepting tasks they told you to), never your own work.
6. Commit after every completed Dispatch task.
7. Use the Dispatch task title as the commit message.

The storage file is an implementation detail. Do not inspect it with `cat`,
`sed`, `jq`, `rg`, editors, scripts, or JSON parsers. If you need task state,
ask the CLI (add `--json` when you need to parse it).

## Find The Workflow Directory

Dispatch state normally lives one directory above the product repository:

```text
work_dir/
  .git/
  dispatch.json
  repo/
```

Run Dispatch commands from `work_dir`. If your session started inside the
product repository or one of its task worktrees, the board is usually in the
parent of the main checkout: `cd` there, or call the binary by absolute path,
and check that `dispatch repo` prints the repository you expect. Do not run
`dispatch init` unless the user asks for a new board.

## Use The Dispatch Skills

Load the matching skill before you act on the board:

| Skill | Use it to |
| --- | --- |
| `dispatch-plan` | plan, break down or organise work into groups, tasks, dependencies and review gates |
| `dispatch-work` | start, continue or finish a task: session startup, workspaces, one commit per task, review gates, failure handling |
| `dispatch-board` | report on the board, ready or review tasks the user decided on, check health and tidy up; its `references/commands.md` lists every command |

## If The Skills Are Missing

`dispatch skills list` shows where the skills are installed. If your runner
does not offer them:

- From the workflow directory, `dispatch skills install` installs them there
  for Claude Code, Codex and opencode.
- A session started inside the product repository or a task worktree outside
  the workflow directory will not look there; `dispatch skills install --global`
  installs them for every session of each runner.
- For runners without skill support, `dispatch skills show <name>` prints a
  skill's `SKILL.md` to stdout.

If you cannot install or read them, ask the user before changing the board. Do
not work from memory of older, longer versions of this file.
