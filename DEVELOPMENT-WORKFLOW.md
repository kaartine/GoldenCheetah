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

## Opt-in memory checks (Linux)

Memcheck is a separate correctness gate, not a frame-time benchmark. Build the
selected QtTest project in a task-owned shadow directory with the matching Qt
qmake, `CONFIG+=force_debug_info`, and no sanitizer instrumentation. Keep the
ordinary native test run as a separate prerequisite. ASan/UBSan and Memcheck
complement each other; an ASan run with leak checking disabled is not leak
verification.

The additional Debian/Ubuntu packages are kept in one installable list:

```bash
sed '/^[[:space:]]*#/d; /^[[:space:]]*$/d' .github/scripts/memcheck-packages.txt |
  xargs sudo apt-get install --no-install-recommends -y
```

Run an already-built native QtTest binary, optionally selecting test functions:

```bash
QT_IM_MODULE=compose python3 .github/scripts/run-memcheck.py \
  --output /path/to/new/memcheck-results --timeout 600 \
  -- /path/to/build/testWorkoutGameCanvas
```

The output directory must not already exist. Logs, QtTest and Memcheck XML,
`summary.json`, and `junit.xml` remain available on failure. Passing requires
normal process exit, completed reports, successful test initialization and
cleanup, at least one passing non-lifecycle test, and no memory errors or
definite/indirect/possible leaks. Reachable allocations remain visible but are
not classified as leaks. Child executables are not instrumented. Timeouts and
interruptions terminate the owned process group and fail the gate. The wrapper
ignores user Valgrind options/rc files; installation-default suppressions and
their match counts remain recorded. It accepts `--valgrind /path/to/valgrind`
and inherits `VALGRIND_LIB` for a locally extracted distribution package.
Memcheck XML is parsed incrementally with a 256 MiB limit, including bytes
actually read; QtTest and UI XML retain a 64 MiB limit. A complete parse through
EOF is required even after a `FINISHED` record. Oversized or malformed reports
fail the gate rather than discarding findings.
The instrumented child uses a private `077` umask so synthetic credential
fixtures do not inherit group-writable defaults from a developer's shell.
For `tst_credentialSettings`, place the output under a private `/tmp` parent
created with `mktemp -d`; workspace ancestors may be group-writable and are
intentionally rejected by the existing credential-directory checks. Do not
relax those checks to make the tests pass.

For `testWorkoutGameRunner`, `GC_TEST_TIMEOUT_SCALE=20` may extend asynchronous
frame waits under instrumentation; the accepted integer range is 1–60 and the
default is 1. Polling cadence and simulation timing do not change. The native
throughput case `publishesLatestFixedStepFrameWithoutGuiDrivenSimulation` must
still run uninstrumented; do not loosen it to accommodate Memcheck. Likewise,
select bounded ViewModel cases rather than claiming that a timed-out long-course
or GPU-performance test passed. The wrapper records passed and skipped coverage.

`QT_IM_MODULE=compose` excludes the desktop ibus input-method integration from
these rendering tests. It is an explicit coverage choice, not a fix for ibus.

Both wrappers use `--smc-check=all` for generated/self-modifying code. Qt's
regexp JIT can also produce symbol-less conditional-jump reports; an explicit
`QT_ENABLE_REGEXP_JIT=0` diagnostic run excludes that JIT backend, not regexp
matching itself. The wrappers record this setting but never set it implicitly.
Preserve the original failure and compare runs before attributing findings to
JIT. See the [Valgrind Qt FAQ, section 5.4](https://valgrind.org/docs/manual/faq.html)
and [Qt regexp debugging guidance](https://doc.qt.io/qt-6.8/qregularexpression.html#debugging-code-that-uses-qregularexpression).

For direct production-chart ownership assertions against an already completed,
matching native application build, see the opt-in
[chartOwnership fixture](unittests/Charts/chartOwnership/README.md). It reuses
production objects without entering application `main()`, checks actual object
destruction, and keeps its outputs separate from the application build. Run it
in the documented disposable runtime; it does not replace the full UI lifecycle
or graceful-shutdown Memcheck gates.

### Memcheck exceptions

The default run has no project suppressions. Qt 6.8.3 on Ubuntu 24.04/glibc 2.39
with Valgrind 3.22.0 can leave 368 bytes of private GUI-pool thread-local storage
classified as possibly lost at Canvas shutdown. A controlled comparison using
the same Canvas objects and 63 passing tests retained the report with ordinary
teardown and public global-pool joining; joining the **private** GUI pool before
QApplication destruction removed it. That private Qt API is diagnostic only and
must not become a production dependency. This does not identify a lost
GoldenCheetah allocation or justify suppressing unrelated leaks.

After preserving the unsuppressed report, that exact TLS allocation path can be
excepted explicitly with
`--suppressions .github/scripts/qt-6.8.3-gui-tls.supp`. The filter matches only
possible leaks through the full TLS/thread-pool chain into this QtGui version;
invalid accesses and definite/indirect leaks still fail. Report such a run as
**passed with a reviewed Qt exception**, not zero findings. The file is versioned
in Git; its path and actual matched names/counts are retained in the report.
Re-run without it after changing Qt, libc or Valgrind. Do not add broad Qt,
thread, Python or system-library suppression patterns.

### Packaged application memory checks

The isolated UI runner supports `GC_UI_MEMCHECK=1` and optional
`GC_UI_VALGRIND=/path/to/valgrind`. Extract the AppImage to a dedicated directory,
set `GC_UI_APPDIR` to that runtime, and pass its native `GoldenCheetah` ELF to
`run-pre-release-ui.sh`. Do not instrument the AppImage launcher, shell, or
accessibility Python process. Select the training lifecycle plus
`graceful_shutdown_request`; the import prerequisite is added automatically.
The new `artifacts/memcheck` directory contains the application memory gate in
addition to the ordinary UI reports. Both must pass, including a reaped normal
application exit and completed Memcheck report. Killed/incomplete application
runs are diagnostic evidence, never leak-free acceptance. Set
`GC_UI_TIMEOUT_SCALE=20` to extend asynchronous UI waits under instrumentation
(startup 30 to 600 seconds, shutdown 8 to 160 seconds). The accepted integer
range is 1–60; unset means 1, and invalid values fail early. Polling, game run
duration and performance thresholds are unchanged. Keep an overall process
timeout too: nested operations/retries are not a single end-to-end deadline.
Normal UI timeouts can expire under instrumentation; preserve that failure
instead of treating absence of a report as success. Never run these fixtures on a real athlete or
trainer, and distinguish reduced startup configurations from normal packaged
application coverage.

The runner/helper regression tests are part of the existing
`unittests/Build/ciTestRunner` qmake `make check` target. They exercise malformed
and missing reports, memory/test failures, empty/skipped coverage, stale output,
process-group cleanup and signals. These orchestration tests do not mean every
CI job runs the expensive native Memcheck gate.

Reference: [Valgrind core manual](https://valgrind.org/docs/manual/manual-core.html)
and [Memcheck manual](https://valgrind.org/docs/manual/mc-manual.html).

## Build and artifact ownership

- Give every build and test run a task- or revision-specific output directory.
  Do not reuse another developer's active build directory.
- For automatic local retention, create a dedicated, per-owner build-only
  workspace (mode `0700`). It must contain only the GoldenCheetah checkout and
  its revision-named `GoldenCheetah-output-*` directories. Never use an athlete
  library, home directory, `.goldencheetah` data directory, or a parent that
  contains any other user data. Mount only this build-only workspace into the
  release container:

  ```bash
  workspace_root=/path/to/build-only/goldencheetah-appimage
  install -d -m 0700 "$workspace_root"
  source_root="$workspace_root/GoldenCheetah-src"
  # Clone or move a clean checkout into "$source_root" before continuing.
  revision=$(git -C "$source_root" rev-parse --short=7 HEAD)
  output_root="$workspace_root/GoldenCheetah-output-$revision"
  mkdir -m 0700 "$output_root"
  docker run --rm --cpus=12 \
    -e GC_BUILD_JOBS=12 \
    -e GC_APPIMAGE_OAUTH_POLICY=unconfigured \
    -v "$workspace_root:/workspace" \
    goldencheetah-release-jammy:20260801 \
    /workspace/GoldenCheetah-src/appveyor/linux/reproduce-appimage.sh \
    /workspace/GoldenCheetah-src \
    "/workspace/GoldenCheetah-output-$revision"
  ```

  After a successful reproducible build, the driver retains that build and the
  newest previous valid tool-owned build. It also removes older tool-owned
  valid builds, marked failures, and interrupted `building` generations whose
  per-output process lock is no longer held. A concurrently running build holds
  its lock and is never pruned. Directories without the exact ownership marker,
  outputs outside the source checkout's parent, unsafe links and foreign-owned
  entries are never adopted for deletion. Legacy unmarked outputs therefore
  require an explicit, path-by-path manual decision.
- The ownership marker binds each managed output to the checkout's canonical
  path, device and inode. Moving the checkout, changing the container mount
  path, or replacing the checkout makes existing generations ineligible for
  automatic retention; inspect and handle those paths manually.
- Local retention validates the complete candidate set before deleting any
  entry, accepts only its marker, lock and four named release artifacts, rejects
  nested mountpoints, and retries once. Deletion is intentionally not
  transactional: an I/O error can leave a partially emptied marked directory,
  which later runs fail closed on rather than adopting. If both attempts fail,
  the build command exits nonzero while preserving the newly completed valid
  output for inspection; do not treat that run as fully successful until the
  unsafe entry or filesystem problem has been resolved and retention rerun by
  a later build.
- Retention performs a lightweight identity and digest check; it does not
  replace the full AppImage package, SBOM and runtime verification required
  before publishing a release.
- Keep the newest verified release, its previous rollback release and the
  reports needed for review. The AppImage promotion store prunes older inactive
  generations after a successful promotion.
- Remove disposable shadow builds after the tests that need them. Keep Docker
  toolchain images when they are the pinned input for future reproducible builds.
- Never clean a shared workspace based on a broad glob. Resolve and review the
  exact paths first, and preserve athlete data and other user-owned files.

## Integrity and provenance

Git commit IDs identify files committed in a source snapshot. Do not duplicate
those file hashes in an asset inventory or CI policy. Changes to project-owned
workflows, build scripts and packaging files use the normal pull-request review,
tests and Git history instead of a separately maintained hash authorization.

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
