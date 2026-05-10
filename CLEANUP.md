# Repository Cleanup Inventory

This inventory lists files that are candidates for removal or untracking after
the Dispatch migration. No files were deleted as part of this inventory.

## Workflow-Only Files

These files support the temporary Canvas/Kanvas planning workflow, not the
Dispatch CLI itself:

- `Project.canvas`: visual planning board used during the migration.
- `canvas-tool.py`: helper CLI for editing the Canvas board.
- `AGENTS.md`: Canvas workflow instructions for agents.
- `RULES.md`: migration/workflow rules that are separate from Dispatch.
- `.obsidian/`: Obsidian vault settings and Canvas watcher plugin files.
- `.session`: untracked local session metadata.

Current decision: keep Kanvas/Canvas information for now so the project does not
lose planning context. Do not remove `Project.canvas`, `canvas-tool.py`,
`AGENTS.md`, `RULES.md`, or the Obsidian plugin files until the user explicitly
asks for that cleanup.

Local-only workflow state, such as `.codex`, `.session`, and
`.obsidian/workspace.json`, should remain untracked.

## Generated Build Artifacts

These files are build outputs or bootstrap artifacts and should not usually be
tracked as source:

- `build/`: object files and compiled binaries.
- `build/*.o`: compiled object files.
- `build/dispatch`: compiled Dispatch binary.
- `dispatch`: generated symlink to `build/dispatch`.
- `nob`: compiled bootstrap binary.
- `nob.old`: temporary backup created when `nob` rebuilds itself.

Current action: generated artifacts are ignored by `.gitignore` and should not
be tracked. Keep `nob.c` and `nob.h` tracked as source.

## Runtime State

- `dispatch.json`: local Dispatch board state.

Current action: the repository-root runtime board is ignored by `.gitignore`.
Tests already use temporary boards and fixture files.

## Legacy TDL Source

These files belong to the old TDL command surface. The active entrypoint now
uses Dispatch commands, but `nob.c` still compiles every C file under `src/`, so
removing them should be paired with a build-script update:

- `include/memory.h`
- `include/parser.h`
- `include/task.h`
- `include/utils.h`
- `src/memory.c`
- `src/parser.c`
- `src/task.c`
- `src/utils.c`

Recommended action: remove after updating the build to compile only active
Dispatch source files and rerunning the automated tests.

## Keep

These files are part of the Dispatch project or current documentation:

- `CLAUDE.md`
- `FEATURES.md`
- `README.md`
- `TEST_PLAN.md`
- `include/dispatch.h`
- `include/dispatch_cli.h`
- `include/dispatch_store.h`
- `src/dispatch.c`
- `src/dispatch_cli.c`
- `src/dispatch_store.c`
- `src/main.c`
- `nob.c`
- `nob.h`
- `tests/fixtures/`
- `tests/run_cli_tests.sh`

## Proposed Cleanup Order

1. Add ignore rules for generated artifacts and local session/editor state.
2. Remove tracked build outputs and runtime board state from git.
3. Remove Canvas/Kanvas workflow files after user approval.
4. Update the build to compile Dispatch-only source files.
5. Remove legacy TDL source and headers.
6. Run `tests/run_cli_tests.sh`.
