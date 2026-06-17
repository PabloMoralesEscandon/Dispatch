# Dispatch Agent Instructions

Dispatch is a command-line workflow board for humans and agents. It organizes
work into groups, tasks, dependencies, assignment, review gates, and completion
history.

These instructions define how agents must use Dispatch in this repository.

Dispatch state normally lives one directory above the repository being changed:

```text
work_dir/
  .git/
  dispatch.json
  repo/
```

Run Dispatch commands from `work_dir`. The `repo/` directory is the product
repository; the parent directory can track workflow state separately.

## Critical Rules

1. Use the Dispatch CLI for all workflow state.
2. Never read `dispatch.json` directly.
3. Never edit `dispatch.json` directly.
4. Never infer task state from `dispatch.json`.
5. Commit after every completed Dispatch task.
6. Use the Dispatch task name as the commit message.

The storage file is an implementation detail. Agents must not inspect it with
commands such as `cat`, `sed`, `jq`, `rg`, editors, scripts, or direct JSON
parsers. If task state is needed, use the CLI.

## Core Commands

Use `dispatch` from the workflow directory, or the absolute path to the binary
if working from another directory.

```bash
dispatch init repo
dispatch ready
dispatch blocked
dispatch list
dispatch tree [group]
dispatch show <TASK-ID>
dispatch start <TASK-ID> --actor <agent-id>
dispatch finish <TASK-ID> --actor <agent-id>
dispatch review <TASK-ID> --actor <user-or-reviewer-id>
dispatch group add <name> --prefix <PREFIX>
dispatch task add <group> <title> [--description <text>] [--no-review]
dispatch dep add <dependency-id> <dependent-id>
dispatch dep remove <dependency-id> <dependent-id>
dispatch normalize
```

## How Dispatch Works

Groups organize related work. Each group has a short prefix, and task IDs use
that prefix, such as `DE-01` or `VD-03`.

Tasks have a title, description, state, dependency list, review setting,
assignment fields, and history. The task title is the human-readable name. The
task ID is the stable identifier agents use in commands.

Dependencies are stored as "this task depends on those task IDs". A task is
blocked when any dependency is not done. Agents must treat blocked tasks as not
available.

The normal lifecycle is:

```text
proposed -> ready -> doing -> review -> done
```

If a task does not require review, `finish` moves it directly to `done`. If it
does require review, `finish` moves it to `review`, and the agent must stop that
sequence until the user or reviewer accepts it.

## Session Startup

At the start of a work session, inspect the board through the CLI:

```bash
dispatch normalize
dispatch ready
dispatch blocked
dispatch tree
```

If the user names a specific task, inspect it before doing anything:

```bash
dispatch show <TASK-ID>
```

If the task is blocked, in review, already assigned, or not ready, explain the
state to the user and do not start it.

## Selecting Work

Only start ready tasks. Do not work on blocked tasks, review tasks, proposed
tasks, or tasks assigned to another agent.

If multiple tasks are ready and the user did not name one, ask which task or
sequence to prioritize. If the user asked you to continue all available work,
choose a ready task that is not assigned and proceed in dependency order.

## Task Execution Protocol

For each task:

1. Inspect it.
2. Start it with your agent ID.
3. Do the work.
4. Run relevant checks.
5. Finish the task through Dispatch.
6. Commit the task result with the task name.
7. Check whether another task in the sequence is ready.

Example:

```bash
dispatch show DE-12
dispatch start DE-12 --actor codex

# Do the code or documentation work.
# Run relevant verification.

dispatch finish DE-12 --actor codex
git status --short
git add <files changed for this task>
git commit -m "DE-12 Implement review-gate behavior"
dispatch ready
```

If `finish` reports that review is required, stop that sequence. Do not continue
past a review gate until the user accepts the task.

If `finish` reports `done` and prints next ready tasks, you may continue only
when the user asked you to continue available tasks. Otherwise report the result
and wait.

## Commit Discipline

Every completed Dispatch task gets its own commit.

Use the task title as the commit message. Include the ID when the title includes
it, matching Dispatch task names such as:

```bash
git commit -m "AD-01 Create CLAUDE.md"
```

Stage only files that belong to the completed task. Do not include unrelated
editor state, generated build artifacts, session files, or another agent's work.

If the worktree already has unrelated changes, leave them alone. Work around
them without reverting them unless the user explicitly asks.

## Parallel Agent Safety

Agents working in parallel must not share the same Git working tree.

Use one Git worktree and one branch per agent or task sequence:

```bash
git worktree add ../Dispatch-agent-codex-DE-12 -b agent/codex/DE-12
```

Dispatch controls task ownership. Git worktrees control file and commit
isolation.

Recommended branch format:

```text
agent/<agent-id>/<sequence-id>
```

Examples:

```text
agent/codex/DE-12-review-gate
agent/claude/VD-02-tests
```

Do not switch branches, stage files, amend commits, or merge branches in a
shared worktree while another agent is using it. A human or designated
integrator should merge completed branches one at a time.

## Planning Work

When the user asks an agent to plan work, create Dispatch tasks through the CLI.
Use groups, meaningful titles, dependencies, and review gates for checkpoints.
Use `dispatch tree` or `dispatch list` to inspect dependency sequences.

Example user request:

```text
Plan the work to add JSON output support.
```

Example agent flow:

```bash
dispatch group add "Development" --prefix DE
dispatch task add DE "Design JSON output contract" --description "Define the machine-readable output shape for list, ready, blocked, and show."
dispatch task add DE "Implement JSON output flag" --description "Add --json output for supported commands."
dispatch task add DE "Test JSON output commands" --description "Verify valid JSON and expected fields."
dispatch dep add DE-01 DE-02  # DE-02 depends on DE-01
dispatch dep add DE-02 DE-03  # DE-03 depends on DE-02
dispatch ready DE-01 --actor user
```

The first task is ready after user approval. The implementation and test tasks
wait behind dependencies.

## Executing A User-Named Task

Example user request:

```text
Do DE-02.
```

Expected agent sequence:

```bash
dispatch show DE-02
dispatch start DE-02 --actor codex

# Make the code changes.
# Run tests or targeted checks.

dispatch finish DE-02 --actor codex
git status --short
git add <relevant files>
git commit -m "DE-02 Implement JSON output flag"
dispatch ready
```

If `dispatch show DE-02` shows blockers, the agent stops and reports the
blockers instead of starting the task.

If `dispatch finish DE-02 --actor codex` moves the task to review, the agent
stops after committing and asks for review.

If finishing the task makes `DE-03` ready and the user asked the agent to keep
going through available tasks, the agent may start `DE-03`.

## Failure Handling

If a task cannot continue because of missing information, a broken assumption,
an external dependency, or a failed check that needs user input, report the
blocker clearly and leave the task assigned. Do not mark a task finished when
the requested work is incomplete.

If a task is finished but needs human acceptance, use `finish` and stop at the
review gate. Do not perform human review on behalf of the user unless the user
explicitly assigned you as the reviewer.
