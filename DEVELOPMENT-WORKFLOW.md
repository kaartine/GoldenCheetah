# Multi-developer workflow

This repository is developed by several people and automation workers at the
same time. Keep work isolated, reviewable and reproducible so that one task
does not silently overwrite another.

## Isolate each change

- Use one branch and one worktree per task. Do not share a writable worktree.
- Start from the current integration branch and fetch before creating the task
  branch. Rebase before handoff, but do not rewrite a branch another developer
  is already consuming without coordinating first.
- Record the task, branch, affected subsystem and expected files in the team
  tracker before editing. Explicitly agree on ownership when two tasks need the
  same file.
- Prefer small, cohesive commits. Keep generated files with the source change
  that requires them and keep unrelated cleanup separate.

Example:

```bash
git fetch --all --prune
git worktree add ../gc-worktrees/ui-filter -b feature/ui-filter master
```

## Coordinate overlapping work

1. Split work by subsystem or file before implementation.
2. If overlap is unavoidable, nominate one developer to own the shared file;
   other developers provide focused commits for that owner to integrate.
3. Rebase the task branch onto the integration branch and resolve conflicts in
   the task worktree.
4. Re-run tests affected by the conflict. A clean textual merge is not proof
   that the combined behavior is correct.
5. Hand off the commit IDs, tests run, artifacts produced and known gaps.

Never use destructive Git commands to discard another worktree's changes.
Inspect `git status`, `git worktree list` and branch ownership before cleanup.

## Test in layers

Run the narrow unit tests while developing, then the subsystem and release
gates before integration. Report only commands that actually ran.

Useful fast checks include:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 \
  unittests/Gui/preReleaseUi/test_analyze_workout_game.py
PYTHONDONTWRITEBYTECODE=1 python3 \
  unittests/Gui/preReleaseUi/test_pre_release_ui_workflow.py
PYTHONDONTWRITEBYTECODE=1 python3 \
  unittests/Build/workoutGameAssets/testWorkoutGameAssets.py
PYTHONDONTWRITEBYTECODE=1 python3 \
  unittests/Build/appImagePackaging/testImmutableActions.py
```

The UI runner accepts a comma-separated filter. It automatically adds known
prerequisites, such as prepared-workout import for Workout Game. Select the
needed cases during development, and run the complete matrix before release:

```bash
GC_UI_TESTS=workout_game_training_lifecycle,graceful_shutdown_request \
  unittests/Gui/preReleaseUi/run-pre-release-ui.sh \
  /path/to/GoldenCheetah.AppImage artifacts/ui-quick-check

unittests/Gui/preReleaseUi/run-pre-release-ui-matrix.sh \
  /path/to/GoldenCheetah.AppImage artifacts/ui-release
```

Xvfb software rendering is a functional and continuity gate. Target frame-time
budgets require `GC_UI_USE_HARDWARE_GL=1`, an explicitly identified GPU and an
accessible test display. Do not describe an Xvfb timing result as target-GPU
acceptance. Existing-display runs share the selected desktop's D-Bus and
`XDG_RUNTIME_DIR` while keeping the athlete library and persistent XDG paths
isolated; prefer a dedicated test login for that hardware gate.

## Build and artifact ownership

- Give every build and test run a task- or revision-specific output directory.
  Do not reuse another developer's active build directory.
- Keep the newest verified release, its previous rollback release and the
  reports needed for review. The AppImage promotion store prunes older inactive
  generations after a successful promotion.
- Remove disposable shadow builds after the tests that need them. Keep Docker
  toolchain images when they are the pinned input for future reproducible builds.
- Never clean a shared workspace based on a broad glob. Resolve and review the
  exact paths first, and preserve athlete data and other user-owned files.

## Integrity and provenance

Git commit IDs identify files committed in a source snapshot. Do not duplicate
those file hashes in an asset inventory. CI trust-boundary inputs are the
exception: their checked-in hashes are authorization records evaluated from the
trusted base branch. Rotate them with the staged contract-only procedure in
`doc/BUILD_ARTIFACT_AUTHENTICITY.md` before changing a protected input.

Cryptographic hashes remain required at trust boundaries: downloaded
dependencies, original external source archives, produced release artifacts,
SBOM linkage and signatures. Asset manifests retain licensing, provenance,
review state, structure and performance budgets; Git provides the identity of
their committed files.

## Integration checklist

- The branch contains only the intended changes and `git status` is clean.
- New behavior and failure cases have automated tests.
- Required narrow, subsystem and release checks passed after the final rebase.
- Documentation describes new switches, operational limits and migration.
- The handoff names the exact commit and any platform or hardware check that
  remains unverified.
- Push the reviewed task branch first. Update a shared integration branch only
  after its current remote tip has been fetched and verified.
