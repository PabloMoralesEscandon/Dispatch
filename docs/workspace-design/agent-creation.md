# Agent Creation Workflow

Task: WD-10

## Decision

Dispatch should create agent identities and local agent support files, but it
should not create a permanent git worktree for each agent. Git worktrees remain
task-scoped.

Agent home directory:

```text
work_dir/
  dispatch.json
  .dispatch/
    agents/
      codex-a/
        AGENT.md
        scratch/
        decisions/
        run.sh
  repo/
  repo-agent-codex-a-WD-01/
```

The shared `dispatch.json` remains the coordination point for all agents. Agent
directories are for prompt material and temporary private state, not task state.

## Commands

Create an agent:

```bash
dispatch agent create --name codex-a --runner codex
dispatch agent create --name claude-a --runner claude --model sonnet
dispatch agent create --name codex-b --runner codex --no-run-script
```

Inspect agents:

```bash
dispatch agent list
dispatch agent show codex-a
dispatch agent command codex-a
dispatch agent command codex-a --print-command
```

`--name` is required. `--runner` is required and initially supports `codex` and
`claude`.

Supported creation options:

- `--model <name>`: pass a model name through to the generated runner command.
- `--no-run-script`: create metadata and prompt files but skip `run.sh`.
- `--print-command`: print the launch command after creation.

No `--purpose` flag is included. The user and Dispatch tasks define what the
agent works on.

## No Agent Worktree By Default

An agent does not get a permanent repository checkout. That would conflict with
the one-worktree-per-task rule and create ambiguity about where edits belong.

Task worktrees are created separately:

```bash
dispatch workspace create WD-01 --actor codex-a
```

This creates a task-specific worktree such as:

```text
repo-agent-codex-a-WD-01/
```

The agent can have many task worktrees over time, but each worktree belongs to a
specific task.

## Generated AGENT.md

`AGENT.md` should tell the agent:

- its agent name;
- it is working with other agents on one shared Dispatch board;
- it must use the Dispatch CLI for workflow state;
- it must never read or edit `dispatch.json` directly;
- it may start only ready, unassigned tasks;
- it must not edit another agent's task worktree or scratch directory;
- it should keep temporary notes in `.dispatch/agents/<name>/scratch/`;
- it should keep agent-local decision notes in
  `.dispatch/agents/<name>/decisions/`;
- it should write project documentation into the repository only when the user
  asks or a Dispatch task explicitly requires it.

## Generated run.sh

`run.sh` should be a convenience wrapper, not a process manager. It starts the
chosen runner with the generated prompt file and the workflow directory as the
current working directory.

Example shape:

```bash
#!/usr/bin/env bash
cd "<work-dir>"
codex --prompt-file ".dispatch/agents/codex-a/AGENT.md"
```

The exact runner flags may differ by tool and should be isolated in runner
templates. If Dispatch cannot confidently generate a runner command, it should
still create `AGENT.md` and print the prompt file path.

## Shared Board And Locking

All agents share one `dispatch.json`. This is safe only when Dispatch mutation
commands use the board lock described in `parallel-safety.md`.

Agents share the board, but they do not share product-repository worktrees.
