# 0005: Portable TUI golden frames

- **Status:** Accepted
- **Date:** 2026-11-16

## Context

GitHub issue #8: `tests/run_tui_golden_tests.sh`, and so the golden step of
`tests/run_cli_tests.sh`, fails on macOS. The goldens were recorded on Linux,
where the fixture lives under `/tmp/dispatch-tui-golden.XXXXXX`.

The runner renders `100x30` frames with `dispatch tui --render-smoke`. It then
replaces the temp root with `<tmp>` in the captured text. That works for a path
that ends its line. It cannot work for a path in the middle of a line that was
already cut to the frame width. The board header
`Repo <path>    N tasks (N shown)    N groups    N agents    N workspaces` is
truncated at column 100 *after* the real path is laid out, so the stats that
survive depend on how long that machine's temp path is. The task inspector's
workspace `Path` field has the same problem when the path is longer than the
field.

On macOS the paths are longer and also spelled differently:

- `dispatch init` stores the repo as a realpath. The default macOS `TMPDIR` is
  under `/var/folders/...`, which resolves to `/private/var/folders/...`. The
  runner's `$TMP_ROOT` substitution never matched it, so the header showed the
  full raw path.
- `workspace create --dir` stores the path as typed, here with the `//` that
  `mktemp` produces from a `TMPDIR` that ends in `/`.

With the default `TMPDIR`, five frames fail: board, help, accented-board,
accented-search and task-inspector. With `TMPDIR=/private/tmp`, only the four
header frames fail, which is what the issue describes.

Two more macOS problems came out of reproducing the issue:

- The mutated-frame check used `sed -i '2s/...'`. BSD sed reads the expression
  as the `-i` backup suffix and fails. So once the goldens pass, the suite's
  "mutation is rejected" assertion would fail too, because sed errors out
  before the runner prints `FAIL: TUI golden frame changed: board`.
- `run_cli_tests.sh` compares typed temp paths with paths that Dispatch,
  `getcwd` and git report as realpaths, for example in `workspace_naming_test`
  and `exec_helper_test`. Under a symlinked `TMPDIR` those comparisons fail.

The issue suggested two fixes:

- **Normalise the captured text.** Not enough on its own: the truncation has
  already happened by then.
- **Build the fixture under a short, fixed, relative path.** Not possible
  either, because `dispatch init` resolves the repo argument to an absolute
  realpath.

A fixed absolute path such as `/tmp/x` is not portable: macOS resolves it to
`/private/tmp/x`, and parallel runs would collide.

## Decision

**Substitute the temp root before layout, in the headless render only.**
`dispatch tui --render-smoke` reads `DISPATCH_TUI_SMOKE_TMP_ROOT`. When it is
set, the render shows any path under that root as `<tmp>/...`. It matches both
the given spelling and the resolved spelling of the root, and only on a path
component boundary. The substitution happens in one display helper used for the
board header and the task and workspace inspectors. The rendered line is then
the same on every machine before it is cut to 100 columns.

The value is kept in `DispatchTui` as display-only state. It never touches
board data, so a render that triggers a save cannot write `<tmp>` into
`dispatch.json`. The interactive TUI never sets it.

**The golden runner exports the variable** for its fixture root. It still
rewrites both spellings of the root in the captured text, for paths at the end
of a line such as the workspaces table. The old help-frame rule that blanked
the header to `Repo <path>` is removed, because the header is now
deterministic.

**Four goldens change on line 6 only.** These are board, help, accented-board
and accented-search. `<tmp>` is shorter than the Linux path the goldens were
recorded with, so more of the header now fits: the line ends in
`N workspaces` instead of a cut-off `N`. The help frame shows the real
`Repo <tmp>/workflow/repo` beside the overlay instead of `Repo <path>`. No other
frame changes, and re-recording on macOS and on a long custom `TMPDIR` gives
identical files.

**The mutation step writes through a temp file** (`sed ... >file.mutated && mv`)
instead of `sed -i`, so it behaves the same with GNU and BSD sed.

**`run_cli_tests.sh` resolves its temp root once** (`pwd -P`) instead of
patching each assertion. Linux CI never has a symlinked temp root, so this
loses no coverage CI had. The suite also checks that the variable is display
only: without it the header shows the real repo path, a symlinked root still
matches, and a root that is only a string prefix of a path component does not.

## Consequences

- Both scripts pass on macOS with the default `TMPDIR`, with
  `TMPDIR=/private/tmp`, and on Linux. Goldens no longer depend on where they
  were recorded.
- `--render-smoke` has one more test-only input. Any new view that renders a
  filesystem path mid-line should go through `display_path`, or its golden will
  depend on the machine again.
- Paths stored outside the fixture root, such as `$HOME`, are not normalised.
  Fixtures must keep everything they render under the runner's temp root.
- Both test scripts call `$BIN` (the built `./dispatch`) directly. The suite
  passes with a failing `dispatch` placed first on `PATH`, so an installed
  binary on a developer machine cannot mask a failure.
