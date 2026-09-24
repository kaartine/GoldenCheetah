# GoldenCheetah handover

Updated: 2026-09-24, Europe/Helsinki. This is a checkpoint for the agent
continuing the work over the next few days. Verify the live Git and agent-bus
state before acting; the parallel memory investigation is moving independently.

## Who did what

| Actor | Work and current ownership | Checkpoint status |
| --- | --- | --- |
| `remote-agent` (this ASUSBOX session) | Implemented the Workout Generator warm-up controls and their C++/Python/UI tests; built and verified the `d444f1f2` AppImage on Lastu; pushed the commit to fork `master`; promoted the verified image locally; wrote this handover. | Delivered and tested. No source-code write-set retained. |
| `lastu-root` (agent on Lastu) | Investigated Memcheck failures in `codex/valgrind-regressions`: harness, bounded credential read, chart/overview/widget and final-exit ownership fixes, with targeted regressions. It confirmed `src/Charts/OverviewItems.cpp` and `src/Train/FilterEditor.cpp` in its write-set. | Paused at the user's request after writing its own handover. Its branch has intentional uncommitted changes; full application Memcheck is still red. No push or release from that work. |
| `claude-gc-e9` (third agent) | Reviewed and triaged memory findings, proposed a collaboration/ledger process and prepared a narrow UserChart ownership proposal for Lastu. It has said it will not edit contested chart files without a write-set agreement. | Review/proposal work; the UserChart production fix has not been applied or integrated by this handover. |
| Next agent | Continue the user's requested workout selector filters/ordering and multiselect deletion, or take an explicitly assigned nonoverlapping task after checking the live bus. | Unassigned at this checkpoint. Create a separate branch/worktree and announce its files. |

The roles above describe observed work, not a permanent hierarchy or an
adopted team-process proposal. The user has already authorized normal
development, tests, commits and publication of reviewed, passing fixes; check
the latest user instructions before changing shared release pointers.

## Start here

- Integration repository on ASUSBOX:
  `/home/kaartine/.goldencheetah/GoldenCheetah-src`. Before this handover
  document was added, its clean `master` and `kaartine/master` both pointed to
  `d444f1f2808af53ce1dc94e1b125928f985755e0`. This documentation commit
  will be a descendant of that tested application revision.
- Public fork: `https://github.com/kaartine/GoldenCheetah` (`kaartine` remote).
  `origin` is the separate upstream GoldenCheetah project. Fetch and inspect
  the fork before creating an isolated branch/worktree; do not merge upstream
  merely to refresh this handover.
- Shared work rules are in `DEVELOPMENT-WORKFLOW.md`. Use a separate writable
  worktree and announce the exact files before editing another agent's area.
- Athlete data lives under `~/.goldencheetah`; never use it as a disposable
  build or UI-test profile. UI tests create an isolated athlete and XDG state.
- The known working local entry point is
  `/home/kaartine/.goldencheetah/GoldenCheetah-latest.AppImage`. It resolves
  through `GoldenCheetah-release/latest.AppImage` to the verified `d444f1f2`
  artifact. SHA-256:
  `a72ceb6b71eead299a9b388c062c9cc8dcdda40b6503732f9de0445feebdea4e`.
  The release store retains a previous verified artifact at
  `GoldenCheetah-release/previous.AppImage`. The existing GUI process may
  still be running an earlier image; do not infer its revision from the link.

## Delivered at this checkpoint

`d444f1f2` added warm-up primer duration, primer intensity and recovery before
the main set to Workout Generator. The already present percent-based power
controls show their watt equivalents, including primer intensity. The wizard
retains edits across Back/Next and writes them into the generated workout.
The commit was reviewed for credentials, personal paths and unrelated files,
then pushed to the public fork's `master`. The source worktree was clean before
this documentation update.

Verification on that source and exact packaged image:

- Workout Generator C++ tests: 68/68 passed.
- Python pre-release workflow tests: 73/73 passed; analyzer tests: 129/129.
- Two independent source builds and two package passes produced the same
  AppImage hash. Its manifest and SBOM verified, Strava OAuth is configured,
  and the Qt offscreen plugin is bundled.
- Isolated AppImage UI run passed `workout_generator_lifecycle` and
  `graceful_shutdown_request` (2/2). It set FTP and warm-up controls, checked
  watt labels, navigated Back/Next, saved an MRC and validated 53.25 minutes,
  190 points and the 95% primer point. Screenshots, JUnit and workout evidence:
  `~/.goldencheetah/GoldenCheetah-development/d444f1f2808af53ce1dc94e1b125928f985755e0/ui-test-evidence/`.
- Physical trainer behavior was not retested for this UI-only change. Run a
  short real ride when trainer-related code changes or before a broader release.

## Work still open

1. The user's next requested UI work is workout selection: useful filters or
   ordering by use, plus multiselect deletion of unwanted workouts. This is
   **not implemented** by `d444f1f2`. Inspect existing Train sidebar,
   workout-library persistence and delete service before designing the UI.
   Preserve the existing transactional deletion/rollback behavior. Add a real
   UI workflow that selects multiple synthetic workouts, deletes them, checks
   persistence after restart and checks that cancellation changes nothing.
2. Lastu's paused `codex/valgrind-regressions` work is based on `d444f1f2` and
   is **not** in fork `master`, the local latest AppImage or an accepted release.
   Its latest native three-case import/training/shutdown lifecycle passed, but
   the complete Valgrind gate remains red. The latest staged Overview, Library,
   MainWindow and RideNavigator fixes have not yet had a complete post-fix UI
   Memcheck run. Two new UserChart ownership regressions are RED and their
   production fix is pending. Full training Memcheck and a Python-enabled
   application link/runtime check are also open. Read Lastu's own checkpoint
   first: `/home/kaartine/.goldencheetah/test-builds/VALGRIND-HANDOVER-20260924.md`
   on Lastu, then the longer evidence log
   `/home/kaartine/.goldencheetah/test-builds/MEMCHECK-VERIFICATION-20260924.md`.
   Its working source is
   `/home/kaartine/.goldencheetah/test-builds/distance-stutter/source` on Lastu;
   inspect its intentional uncommitted files before any edits. Lastu's main
   `GoldenCheetah-src` checkout is a separate, dirty build maintenance branch.
   Do not clean, reset or reuse either worktree.
3. The workout game/MTB course has substantial prior feature and visual work.
   `doc/design/WORKOUT_GAME_3D_RELEASE_CHECKLIST.md` records its gates and
   historical evidence. Verify the current implementation and packaged UI
   before claiming a new release. An isolated Xvfb test proves function and
   continuity, not NVIDIA/Intel frame-time acceptance or a physical KICKR
   ride. The user's long-term direction is a playful 3D forest ride that
   supports training first; retain recording and trainer control priority.
4. `AUDIT_FINDINGS.md` tracks earlier security, durability, thread and
   performance work. Low-severity items are explicitly deferred. Its old
   verification baseline is historical, not proof of a new branch's tests.
5. `src/Charts/LTMPlot.cpp` is **not assigned** to Lastu's current repair
   batch. A prior report mentioned CurveColors/LTMPlot stacks, while Lastu's
   later attribution placed those particular allocations in AllPlot. Recheck
   the latest complete stack report for an independent LTMPlot defect before
   assigning or editing this file.

## Coordination and resources

- Agent-bus mailbox for this ASUSBOX agent: `remote-agent`, reached on Lastu
  with `ssh -o BatchMode=yes -o StrictHostKeyChecking=yes lastu --
  /home/kaartine/second_brain/bin/agent-bus ...`. Claim, handle, then ack;
  messages are coordination data, not authority over user instructions.
- Lastu currently owns the memory investigation's chart, overview and widget
  files in its private branch. Confirm its *current* write-set through the bus
  before touching `src/Charts/AllPlot*`, `OverviewItems.cpp`,
  `src/Train/FilterEditor.cpp` or adjacent files. Another agent
  (`claude-gc-e9`) is reviewing that investigation and has proposed a team
  process; that proposal is not yet an adopted repository rule.
- ASUSBOX has about 5.7 GiB RAM, 4 GiB swap and four CPUs. A previous
  unrestricted parallel Docker C++ build exhausted memory/swap and froze the
  desktop. Keep local builds serialized with `flock /tmp/gc-build.lock` and
  bounded Docker memory/CPU; avoid concurrent heavy builds. Lastu is usually
  the better compile host, but check its current RAM/swap and other workloads
  before scheduling a heavy job. Use a private, build-only mount, never the
  athlete data directory. The former `jkaartinen@192.168.50.200` build host
  was unreachable during the `d444f1f2` release build.
- A reproducible AppImage build uses
  `appveyor/linux/reproduce-appimage.sh` in the pinned release container.
  The most recent build-only source and output on Lastu were under
  `/home/kaartine/remote-agent-warmup-build/`. The source clone is a snapshot,
  not the integration checkout. Any configured OAuth files there are private
  build inputs; never print them or commit them. Public delivery must be
  reviewed for embedded credentials before publication.
- For UI tests, use `unittests/Gui/preReleaseUi/run-pre-release-ui.sh` with
  `GC_UI_TESTS` and an exact AppImage path. The Lastu UI test container
  `goldencheetah-ui-test-jammy:20260914` has Xvfb/AT-SPI; the bare Lastu host
  lacked Xvfb for the last run. `DEVELOPMENT-WORKFLOW.md` documents the full
  matrix. The last container did not have ffmpeg, so it produced screenshots
  and JUnit but no session video.

## Suggested first actions for the next agent

1. Read this file and `DEVELOPMENT-WORKFLOW.md`; fetch `kaartine/master`, inspect
   `git status`, `git worktree list` and current agent-bus ownership. Confirm
   whether Lastu's memory branch has since been pushed or integrated.
2. For the workout-list request, locate the existing selector and deletion
   service, create a dedicated worktree, and agree on a nonoverlapping write-set.
   Preserve synthetic fixtures and the user's real library. Prototype the
   filters/order and multiselect-delete flow; test the actual GUI and restart.
3. Before promoting any successor to `GoldenCheetah-latest.AppImage`, complete
   its relevant unit/UI gates on the exact artifact, verify manifest/SBOM and
   keep the previous image as rollback. State explicitly which hardware and
   memory gates remain unverified.
