# GoldenCheetah Code Audit Findings

This file tracks the findings from the July 2026 local audit of the
GoldenCheetah source tree. It is intentionally kept in the repository so each
finding can be tied to a regression test, a focused fix, and a commit.

Baseline: `44fcbb8` (`master`)

## Workflow

For every finding:

1. Add a regression test that demonstrates the failure or unsafe input.
2. Confirm the test fails for the expected reason.
3. Implement the smallest complete fix.
4. Run the focused test and the full unit-test suite.
5. Review the diff for credentials, personal paths, and unrelated changes.
6. Commit and push the finding independently where practical.

Statuses are `OPEN`, `IN_PROGRESS`, `FIXED`, `DEFERRED`, or `NOT_REPRODUCIBLE`.

## Critical

### SEC-001: Remote default layouts can execute Python or R code

- Status: FIXED
- Code: `src/Gui/AbstractView.cpp`,
  `src/Gui/PerspectiveStateSource.cpp`,
  `src/Gui/PerspectiveStateSource.h`,
  `src/Charts/PythonChart.cpp`
- Impact: Default perspective XML is downloaded over HTTP. The XML selects a
  chart type and sets arbitrary string properties, including executable Python
  and R chart scripts. A network attacker can gain code execution when a user
  resets layouts to defaults.
- Test: Feed an untrusted default layout containing a script property and
  verify that remote state cannot instantiate or configure scriptable charts.
- Fix direction: Package trusted defaults locally. If remote defaults remain,
  require HTTPS plus an authenticated manifest and apply a strict schema that
  excludes executable chart types and properties.
- Resolution: Layout reset no longer performs a network request. The new
  `PerspectiveStateSource` trust boundary loads reset state only from the
  packaged `:/xml` resources, whitelists the four application view names, and
  keeps normal saved-state loading explicit.
- Verification: The regression test first failed because the trusted loader did
  not exist. All five QtTest cases pass in normal and
  ASan/UBSan/LSan-instrumented builds. The application links, and the full test
  matrix passes 79 test programs and 2,697 tests with zero failures or skips.
  The reset path contains no network API references, and all four whitelisted
  perspective files are present in the packaged resource manifest.

### SEC-002: ZIP extraction permits path traversal and symlink escape

- Status: FIXED
- Code: `contrib/qzip/zip.cpp`, `contrib/qzip/zipreader.h`,
  `src/FileIO/ArchiveFile.cpp`, `src/FileIO/ArchiveFile.h`
- Impact: Archive member paths and symlinks were used without rejecting
  absolute paths, `..`, or links escaping the destination. Activity and plan
  imports could overwrite arbitrary files writable by the user.
- Test: Cover absolute, drive-prefixed, backslash, traversal, dot-component,
  reserved-device, trailing-dot, archive-symlink, destination-symlink, root
  symlink, existing-file, case-mismatch, Unicode-normalization, corrupt-data,
  central-directory corruption,
  selective-extraction, rollback, and valid nested-file cases.
- Resolution: Extraction now validates and NFC-normalizes every member before
  changing the filesystem, rejects non-portable aliases and links (including
  Windows reparse points), detects case-folded collisions, and refuses existing
  file targets. Selected members are matched exactly, decompressed and CRC
  checked before writes, written through `QSaveFile`, and rolled back together
  with newly created directories on later failure. Existing directory
  permissions are never changed.
- Verification: The focused QtTest suite first failed on the vulnerable cases
  and now passes 26 tests. The same 26 tests pass under ASan and UBSan, the
  application builds, and the full suite passes 113 tests with zero failures.
- Scope: Archive resource-exhaustion limits remain tracked separately as
  `PARSE-001`.

### SEC-003: Icon bundles permit arbitrary file overwrite

- Status: FIXED
- Code: `src/Gui/IconManager.cpp`, `src/Gui/IconManager.h`,
  `src/Gui/Pages.cpp`
- Impact: `icons.zip` was downloaded over HTTP and each member name was passed
  to `QDir::absoluteFilePath` without containment checks. Both downloaded and
  local bundles could overwrite arbitrary user files.
- Test: Reject traversal, absolute paths, archive links, aliases, corrupt data,
  invalid mappings and SVGs, destination links, promotion-time redirection,
  cleartext HTTP, HTTPS downgrade, and TLS errors without changing icon state.
- Resolution: Bundle members are validated and extracted into temporary
  staging before atomic per-file promotion. Destination ancestors and targets
  are checked for links or Windows reparse points immediately before each
  commit, failures roll back, and `mapping.json` is published last. Downloads
  require peer-verified HTTPS with no downgrade and a final 2xx response.
- Verification: The focused normal and ASan/UBSan suites each pass 36 tests,
  the aggregate suite passes 317 tests, and `IconManager.o` and `Pages.o`
  compile in the Qt 6.8.3 development container.
- Residual: A process or power failure between file commits can leave mixed
  icon versions inside `.icons`. Cross-file crash atomicity would require
  immutable versioned bundles plus one atomic pointer or manifest switch; this
  does not permit writes outside `.icons`.

### MEM-001: WKO import contains attacker-controlled buffer overflows

- Status: FIXED
- Code: `src/FileIO/WkoRideFile.cpp`, `src/FileIO/WkoRideFile.h`
- Impact: File-controlled strings were copied into two 32-byte buffers without
  length checks. A crafted WKO file could corrupt object or stack memory.
- Test: Import WKO fixtures with relevant lengths 31, 32, 254, and 65535 plus
  truncated fields under ASan/UBSan. Reject an oversized sparse input before
  allocation or reading.
- Resolution: Graph and chart names are decoded through end-aware bounded
  helpers that reject oversized and truncated payloads before copying. The
  whole-file read is capped below the parser's bit-offset limit, checks the
  exact byte count, and derives its end pointer from the verified read.
- Verification: The focused normal and ASan/UBSan suites each pass 17 tests
  without leaks, and the production `WkoRideFile.o` target compiles.

### MEM-002: Short ANT burst frames become a huge memcpy

- Status: FIXED
- Code: `src/ANT/ANT.cpp`, `src/ANT/ANTChannel.cpp`,
  `src/ANT/ANTChannel.h`, `src/ANT/ANTMessage.cpp`, `src/ANT/ANTMessage.h`
- Impact: A short burst frame produced a negative payload length that became a
  very large `size_t` for `memcpy`.
- Test: Feed checksum-valid lengths 0-8, 10, and 255 through `receiveByte` and
  the real channel dispatch. Cover malformed-to-valid recovery, exact payload
  copies, sequence mismatch/progression, last-packet reset, and the 128-byte
  assembly boundary under normal and ASan/UBSan builds.
- Resolution: Standard burst frames now require the exact nine-byte ANT data
  length before logging or dispatch. A checked payload view exposes exactly
  eight bytes. Channel assembly uses bounded unsigned copy lengths and resets
  partial state when a packet arrives out of sequence.
- Verification: The focused normal and ASan/UBSan suites each pass 39 tests
  without sanitizer findings.

### MEM-003: Malformed Kinetic BLE notifications access memory out of bounds

- Status: FIXED
- Code: `src/Train/BT40Device.cpp`, `src/Train/KurtInRide.cpp`,
  `src/Train/KurtInRide.h`, `src/Train/KurtSmartControl.cpp`,
  `src/Train/KurtSmartControl.h`
- Impact: InRide always read 20 bytes, while an empty Smart Control packet
  underflowed `size - 1` and accessed memory before an empty allocation.
- Test: Exercise every exact-sized payload from 0 through 21 under normal and
  ASan/UBSan builds, while verifying invalid parses leave output unchanged.
- Resolution: Bounded parsers enforce each protocol's packet-size contract
  before decoding. All four BT40 notification branches return before changing
  telemetry or calibration state when parsing fails.
- Verification: The focused normal and ASan/UBSan suites each pass 112 tests,
  and `KurtInRide.o`, `KurtSmartControl.o`, and `BT40Device.o` compile.

### BLE-001: VO2 reconnect uses a dangling static widget pointer

- Status: FIXED
- Code: `src/Train/BT40Device.cpp`, `src/Train/BT40Device.h`,
  `src/Train/VMProWidget.h`
- Impact: A function-static `VMProWidget` is parented to the first BLE device.
  Destroying the device frees the widget but leaves the static pointer, so the
  next reconnect calls through freed memory. Multiple VO2 devices also share it.
- Test: `unittests/Train/vmProWidgetLifecycle` verifies same-device reuse,
  independent widgets for two devices, selective owner destruction, and clean
  recreation after the original owner is gone.
- Resolution: Each `BT40Device` now stores its own guarded widget pointer. The
  shared create-or-reconnect contract reuses only that device's live child and
  creates a new child after Qt clears the pointer during parent destruction.
- Verification: The regression test first failed because the per-device
  contract did not exist. All 5 cases pass normally and under strict
  ASan/UBSan/LSan, the full registered test suite passes without failures or
  skips, and the full Qt 6.8.3 application build links successfully.

## High

### DUR-001: Activity saves are non-atomic and report false success

- Status: FIXED
- Code: `src/FileIO/AtomicFileWriter.h`, `src/FileIO/JsonRideFile.y`,
  `src/Gui/SaveDialogs.cpp`, `src/Core/RideCache.cpp`,
  `src/Core/RideCacheRemoval.cpp`, `src/Gui/SplitActivitySave.cpp`,
  `src/Gui/SplitActivityWizard.cpp`, `src/Gui/MergeActivityWizard.cpp`,
  `src/Charts/AerolabWindow.cpp`, `src/Core/GcUpgrade.cpp`,
  `src/Gui/RideImportWizard.cpp`, `src/Gui/DownloadRideDialog.cpp`,
  `src/Cloud/CloudService.cpp`
- Impact: JSON save truncates the destination in place, does not check write or
  flush status, and callers mark the activity clean even after failure. Disk
  exhaustion or a crash can destroy the only good copy.
- Test: `unittests/FileIO/atomicActivitySave` covers open, short-write, flush,
  commit, corrupt-readback, lock, collision, staged-set rollback, source-change,
  finalization, retry, dialog, and aggregate-cache failures.
  `unittests/Gui/splitActivitySave` covers all split staging, publication,
  archive, rollback, path, sync, and recovery outcomes.
  `unittests/Core/rideCacheRemoval` executes the production removal code and
  verifies exact named eviction after the source has already been archived.
- Resolution: JSON writes now use same-directory atomic publication, file and
  directory synchronization, readback verification, deterministic per-path
  locks, and error propagation. Activities are marked clean only after durable
  publication and source-file finalization. Failed finalization restores the
  source and removes an unfinalized new target. Split Activity now stages and
  synchronizes every output before publishing the set, snapshots and locks the
  source, atomically archives it while preserving any prior backup, and mutates
  the cache only after persistence succeeds. It removes the captured source by
  name, so a nested event loop cannot delete a newly selected activity. Merge
  Activity now saves its isolated replacement candidate first and transfers
  ownership to the current activity only after durable persistence succeeds;
  a failed save leaves the original activity and retry state intact. Aerolab
  now reports failed persistence and leaves its Save action enabled so the
  parameters remain retryable. Legacy activity upgrades now move each source
  into the imports directory only after its JSON replacement is durably
  written, while atomic replacement keeps interrupted upgrades retryable. Ride
  Import now publishes staged JSON before inserting the activity into the
  cache and reports write, publication, and linked-activity save failures.
  Device downloads use the same ordering and preserve stale staging files for
  recovery instead of silently replacing them. Cloud upload and sync stop and
  report local save failures. Cloud downloads publish JSON before updating
  their activity lists or cache, and failed writes no longer create phantom
  in-memory activities.
- Verification: The new regression cases first failed because the staged-set
  finalizer, atomic move, transactional split helper, named archived-cache
  removal, and publication-before-cache contract did not exist. The final
  `atomicActivitySave`, `splitActivitySave`, and `rideCacheRemoval` suites
  pass 71, 33, and 6 tests respectively both
  normally and under strict ASan/UBSan/LSan. The full Qt 6.8.3 application
  build links, and all 1,293 registered tests pass without failures or skips.
- Follow-up: Multi-file crash recovery and rollback against non-cooperating
  writers are tracked separately as `DUR-007` and `DUR-008`.

### DATA-001: Split extraction loses and misaligns boundary data

- Status: FIXED
- Code: `src/Gui/SplitRideData.cpp`,
  `src/Gui/SplitActivityWizard.cpp`
- Impact: Point extraction excludes the stop marker while XData extraction
  includes it, so the final selected sample is omitted and adjacent series use
  inconsistent boundary ownership. XData is copied twice; the second `QMap`
  insertion replaces the first owned pointer without deleting it. An interval
  truncated at the segment end stores the source's absolute stop time instead
  of the segment-local offset.
- Regression test: `unittests/Gui/splitRideData` extracts adjacent and final
  segments containing samples, XData, boundary and crossing intervals, and
  invalid index ranges. It requires exact boundary ownership, one retained
  XData series, and segment-local interval bounds.
- Resolution: A pure segment-copy helper applies an explicit half-open policy
  to preceding segments and includes the selected endpoint only in the final
  segment. It copies XData once, offsets both interval endpoints, preserves
  sub-second start offsets, and rejects invalid ranges.
- Verification: The RED build failed because the helper contract did not
  exist. The focused suite passes 13 tests normally, under strict
  ASan/UBSan/LSan with leak detection, and from an isolated staged-only
  snapshot. The full Qt 6.8.3 application also links from that snapshot.
  The complete worktree run passes 1,306 tests in 30 registered suites
  without failures or skips. A staged-only full-suite rebuild is blocked
  before this suite by the pre-existing `DB-003` `Library::importFiles`
  production/test-stub signature split.

### DUR-002: Other persistent files are also truncated in place

- Status: FIXED
- Code: fixed in `src/Core/Measures.cpp`, `src/Core/Seasons.cpp`,
  `src/Metrics/RideMetadata.cpp`, and `src/Core/RideDB.y`.
- Impact: Measures, seasons, metadata, and cache state can be left empty or
  partial on ENOSPC or process failure.
- Measures regression test: `unittests/Core/measuresAtomicSave` injects open,
  short-write, flush, and commit failures and requires the previous measures
  file to remain byte-for-byte intact. It also validates the successful JSON
  publication path.
- Measures resolution: Serialize the complete document in memory and publish
  it with the atomic persistence helper introduced for DUR-001. Return errors
  to programmatic callers while preserving the existing user-visible dialog
  for callers that do not request an error string.
- Measures verification: The RED build failed because `MeasuresGroup::write`
  had no injectable writer contract. The focused suite passes 7 tests normally
  and under strict ASan/UBSan/LSan with leak detection. The full Qt 6.8.3
  application links, and the complete worktree run passes 1,313 tests in 31
  registered suites without failures or skips.
- Seasons regression test: `unittests/Core/seasonParser` injects open,
  short-write, flush, and commit failures and requires the previous
  `seasons.xml` to remain byte-for-byte intact. The successful path is parsed
  back and checked for complete XML and preserved values.
- Seasons resolution: Build the complete XML document in memory and publish it
  through the atomic persistence helper. Preserve the existing two-argument
  API and user-visible error dialog while allowing tests and programmatic
  callers to receive detailed failures.
- Seasons verification: The RED build failed because `SeasonParser::serialize`
  had no injectable writer contract. The focused suite passes 8 tests normally
  and with production sources under strict ASan/UBSan/LSan. The full Qt 6.8.3
  application links, and the complete worktree run passes 1,318 tests in 31
  registered suites without failures or skips.
- Metadata regression test: `unittests/Metrics/rideMetadataAtomicSave` injects
  open, short-write, flush, and commit failures and requires the previous
  `metadata.xml` to remain byte-for-byte intact. The successful path reads the
  generated XML back through the production parser and verifies escaped
  keywords, fields, defaults, colors, and expressions.
- Metadata resolution: Build the complete XML document in memory and publish
  it through the atomic persistence helper. Return detailed failures to
  programmatic callers while retaining the existing user-visible error dialog
  for the original five-argument calls.
- Metadata verification: The RED build failed because
  `RideMetadata::serialize` returned no result and had no injectable writer
  contract. The focused suite passes 7 tests normally and under strict
  ASan/UBSan/LSan with leak detection; the dependent athlete migration suite
  passes 12 tests. The full Qt 6.8.3 application links, and the complete
  worktree run passes 1,325 tests in 32 registered suites without failures or
  skips.
- Cache regression test: `unittests/Core/rideCacheAtomicSave` injects open,
  short-write, flush, and commit failures and requires the previous
  `rideDB.json` to remain byte-for-byte intact. It also verifies complete
  successful publication through the production persistence helper.
- Cache resolution: Serialize the complete cache document in memory, validate
  the stream, and atomically publish it with the shared persistence helper.
  Preserve the existing public slot while reporting failures and exposing a
  result-returning path for programmatic callers and fault injection.
- Cache verification: The RED build failed because no cache persistence
  contract existed. The focused suite passes 7 tests normally and under strict
  ASan/UBSan/LSan with leak detection. The full Qt 6.8.3 application compiles
  and links with the production parser and cache writer. The complete worktree
  run passes 1,332 tests in 33 registered suites without failures or skips.

### DUR-003: TrainDB drops user tables when version lookup fails

- Status: FIXED
- Code: `src/Train/TrainDB.cpp`, `src/Train/Library.cpp`,
  `src/Train/LibraryImportFileStager.cpp`, `src/Core/GcUpgrade.cpp`
- Impact: A version-table read error was treated as an obsolete schema and could
  drop tags, ratings, last-run state, and other user-maintained tables without a
  transaction or a verified migration.
- RED evidence: An invalid schema still allowed `deleteWorkout()` to remove a
  sentinel row; failed rebuilds still allowed later writes; an import result
  with an incomplete request list could finalize migration; the late-upgrade
  path initially made zero verified-finalization calls and later initialized
  the media library zero times before import; and the file-staging regression
  initially failed to compile because no safe staging contract existed.
- Regression coverage: The TrainDB suite exercises malformed, missing, locked,
  corrupt, unknown, inconsistent, empty, version-one, rollback, changed-plan,
  incomplete-result, UNC, null-path, retry, and post-failure write cases. The
  athlete migration suite verifies the late-upgrade handoff, and the staging
  suite covers copy, identical retry, collision, rollback, missing source, and
  symbolic-link cases.
- Resolution: Schema reads now distinguish empty, current, migration-ready,
  invalid, and I/O-error states. Invalid or failed databases are protected with
  SQLite `query_only`; schema creation and rebuilds are transactional. Legacy
  tables remain intact until an immutable plan, exact request list, successful
  imports, destination rows, and unchanged source rows are all verified in one
  final transaction. The media library is initialized before migration, and a
  video import fails closed if no persistent media-library target exists. File
  staging reuses only byte-identical retry targets and removes only files
  created by a rolled-back attempt. VideoSync retry now checks and updates the
  correct table.
- Verification: TrainDB passes 26 tests normally and under strict
  ASan/UBSan/LSan; athlete migration passes 13 and file staging passes 9 under
  the same sanitizers. The Qt 6.8.3 application compiles, links, and completes
  a `--version` smoke test. The complete worktree run passes 1,368 tests in 35
  registered suites with no failures or skips.

### DUR-004: Full athlete backup is incomplete and not verified

- Status: FIXED
- Code: `src/FileIO/AthleteBackup.cpp`,
  `src/FileIO/AthleteBackupArchive.cpp`, `contrib/qzip/zip.cpp`
- Impact: Planned activities, root-level database state, nested files, and read
  failures may be omitted while backup still reports success. Media is loaded
  fully into memory.
- Regression coverage: The new 16-case `athleteBackupArchive` suite compares an
  exact recursive fixture manifest, covers all SQLite companion files, hidden
  files, source changes, cancellation, symlinks, corrupt archives and payloads,
  source/output/directory write failures, null devices, and preservation of an
  existing destination. The RED test first failed to compile because the
  manifest, streaming, verification, and publication contract did not exist.
- Resolution: Backups now use an explicit persistent-data manifest, recursively
  stream regular files through checked `QIODevice` ZIP APIs, reject unsupported
  links and classic-ZIP limits, and fail closed on every read, write, seek, or
  flush error. The completed temporary archive is checked against the exact
  manifest by size, CRC, and payload before an fsync-backed, no-overwrite atomic
  publication; cancellation and all failures leave an existing target intact.
- Verification: The focused suite passes 16 tests normally and under strict
  ASan/UBSan/LSan. Existing archive and icon-bundle security suites pass 26 and
  36 tests. The Qt 6.8.3 application compiles, links, and completes a `--version`
  smoke test. The complete worktree run, including Python/SIP compilation,
  passes 1,384 tests in 36 registered suites with no failures or skips.

### TRN-001: Device errors automatically delete the current recording

- Status: FIXED
- Code: `src/Train/TrainSidebar.cpp`, `src/Train/TrainSidebar.h`,
  `src/Train/TrainingStopPolicy.h`
- Impact: A late trainer disconnect maps directly to `DiscardRecording`,
  deleting otherwise valid workout data without confirmation.
- Regression test: `unittests/Train/trainingStopPolicy` writes sample CSV data,
  applies the real controller-stop disposition, and requires a device error to
  preserve the exact bytes. It also verifies normal controller completion still
  imports and only an explicit discard removes the file.
- Resolution: Controller failure now stops the session with the `Keep` action,
  closes every recording stream, leaves the raw CSV in the athlete records
  directory, and reports that the partial recording was preserved. File removal
  is confined to the explicit discard action.
- Verification: The RED build failed because the stop-policy contract did not
  exist. All 5 focused tests pass normally and under ASan/UBSan/LSan, the Qt
  6.8.3 application builds and starts, and the complete matrix passes 1,406
  tests in 38 suites with no failures or skips.

### TRN-002: Recording I/O failures are silent

- Status: FIXED
- Code: `src/Train/TrainingRecordingIo.h`,
  `src/Train/TrainSidebar.cpp:1477`, `src/Train/TrainSidebar.cpp:1559`,
  `src/Train/TrainSidebar.cpp:1623`, `src/Train/TrainSidebar.cpp:2485`
- Impact: Failure to create, write, or flush the workout CSV is not surfaced.
  Training continues while the UI implies that data is being recorded.
- Regression test: `unittests/Train/trainingRecordingIo` covers an unwritable
  target, failed and short writes, failed flushes, exact successful writes,
  explicit Stop-time flushing, and first-failure latching. The existing
  `unittests/Train/trainingStopPolicy` suite verifies that keeping a failed
  recording does not remove its raw file.
- Resolution: Main workout CSV creation, every sample write, and explicit
  Stop-time flushing now require exact success. The first failure stops the
  session, preserves an existing partial CSV, and leaves a persistent error
  notification. An open failure reports separately that no recording file was
  created. Auxiliary telemetry files remain covered by their separate audit
  findings.
- Verification: The RED build failed because the checked recording-I/O
  contract did not exist. All 9 focused tests pass normally and under strict
  ASan/UBSan/LSan. The Qt 6.8.3 application builds and starts, and the complete
  matrix passes 1,415 tests in 39 suites with no failures or skips.

### TRN-003: Auxiliary telemetry timestamps become non-monotonic across pause

- Status: FIXED
- Code: `src/Train/TrainingTelemetryTimeline.h`,
  `src/Train/TrainSidebar.cpp:1378`, `src/Train/TrainSidebar.cpp:3530`,
  `src/Train/TrainSidebar.cpp:3546`, `src/Train/TrainSidebar.cpp:3662`
- Impact: RR, position, core-temperature, and VO2 samples continue during pause
  using a timer that is reset on resume. Timestamps can then jump backwards.
- Regression test: `unittests/Train/trainingTelemetryTimeline` exercises all
  four channels before, during, and after pause and calibration. It also
  requires strictly increasing per-channel timestamps when the raw clock stalls
  or moves backwards, independent channel state, session reset, invalid channel
  rejection, and overflow-safe saturation.
- Resolution: All auxiliary streams now use one active-session timestamp source
  and reject samples while stopped, not recording, paused, or calibrating.
  Per-channel high-water marks prevent equal or decreasing timestamps around
  resume boundaries, and the timeline is reset only for a new training session.
- Verification: The RED build failed because the shared telemetry-timeline
  contract did not exist. All 10 focused tests pass normally and under strict
  ASan/UBSan/LSan. The Qt 6.8.3 application builds and starts, and the complete
  matrix passes 1,425 tests in 40 suites with no failures or skips.

### DEV-001: Failed devices are still reported as connected

- Status: FIXED
- Code: `src/Train/TrainSidebar.cpp`,
  `src/Train/BluetoothDeviceTypes.h`
- Impact: Controller start results are ignored and polling begins regardless.
  Failed devices can remain in a connected UI state and emit repeated errors.
- Regression test: `unittests/Train/deviceSelection` uses fake start and stop
  callbacks to require reverse-order rollback of both the failed controller and
  every controller started earlier. It also verifies that successful starts do
  not stop any controller.
- Resolution: Multi-device startup now checks every controller result and rolls
  back the failed controller plus all previously started controllers before the
  connected state or polling timer can be enabled. The UI reports the device
  that failed and clears the active-device set.
- Verification: The focused suite passes 21 tests normally and under strict
  ASan/UBSan/LSan. The full matrix passes 1,401 tests in 37 suites, and the Qt
  6.8.3 application build links and starts successfully.

### BLE-002: FTMS target scaling can divide by zero

- Status: FIXED
- Code: `src/Train/BT40Device.cpp:1080`,
  `src/Train/BT40Device.cpp:1555`, `src/Train/Ftms.cpp:123`
- Impact: FTMS load control is enabled before asynchronous range discovery has
  supplied a positive increment. A delayed or missing response reaches division
  by zero and sends invalid targets.
- Regression test: `unittests/Train/ftmsTargetReadiness` covers delayed and
  absent ranges, zero increments, truncated, oversized, and reversed ranges,
  power and resistance scaling, latest-target replacement, reconnect reset,
  non-finite inputs, numeric extremes, exact command bytes, malformed feature
  data, and invalid command types.
- Resolution: FTMS target commands now pass through a readiness controller.
  Requests made before range discovery retain only the latest target and emit
  no command until an exact six-byte range with ordered limits and a positive
  increment has been accepted. Scaling uses bounded 64-bit intermediates, and
  reconnects and mode changes discard stale state. Feature discovery is also
  sequenced after the control point is initialized and requires an exact
  eight-byte payload.
- Verification: The RED build failed because the readiness contract did not
  exist, and the invalid-enum regression failed before explicit target-type
  validation. All 20 focused tests pass normally and under strict
  ASan/UBSan/LSan, and the related BT40 lifecycle suite passes 17 tests. The Qt
  6.8.3 application builds and starts, and the complete matrix passes 1,445
  tests in 41 suites with no failures or skips.

### BLE-003: Multiple BLE sources overwrite one shared telemetry object

- Status: FIXED
- Code: `src/Train/BluetoothTelemetryRouter.cpp`,
  `src/Train/BT40Controller.cpp`, `src/Train/BT40Device.cpp`
- Impact: A trainer, HR belt, and power meter race by notification timing.
  Disconnecting one source also clears values still supplied by another source.
- Regression test: `unittests/Train/bluetoothTelemetryRouter` interleaves
  physical sources, verifies dedicated-sensor priority, stable equal-priority
  ownership, independent metric owners, stale fallback, source removal, invalid
  input rejection, and router reset. `unittests/Train/bt40Lifecycle` exercises
  every controller telemetry setter against the real `RealtimeData` class and
  verifies fallback and source-specific clearing.
- Resolution: BLE notifications now publish per-device, per-metric snapshots to
  a telemetry router. Dedicated sensors take priority over trainer telemetry,
  current equal-priority owners remain stable while fresh, stale owners fall
  back after five seconds, and disconnecting a device removes only that
  device's snapshots.
- Verification: The RED controller test did not compile before the source-aware
  API and router existed. The focused router suite passes 14 tests and the BT40
  lifecycle suite passes 19 tests, both normally and under strict
  ASan/UBSan/LSan. The Qt 6.8.3 application builds and starts, and the complete
  matrix passes 1,461 tests in 42 suites with no failures or skips.

### BLE-004: BLE service discovery initializes services repeatedly

- Status: FIXED
- Code: `src/Train/BT40Device.cpp`, `src/Train/BT40Device.h`
- Impact: Each service completion loops over every discovered service, repeating
  CCCD writes, FTMS control requests, Wahoo queue resets, and VO2 setup.
- Regression test: `unittests/Train/bt40Lifecycle` completes heart-rate and
  cadence services in both orders, repeats their completion signals, and then
  recreates both services through the reconnect path. It asserts exactly one
  characteristic lookup per service object on each connection.
- Resolution: Service completion now accepts only the signal's service object
  when it belongs to the current connection and has not already been
  initialized. The per-connection initialized set is cleared when stale service
  objects are destroyed during reconnect or shutdown.
- Verification: Both RED rows observed two lookups for whichever service
  completed first. The BT40 lifecycle suite now passes 21 tests normally and
  under strict ASan/UBSan/LSan. The Qt 6.8.3 application builds and starts, and
  the complete matrix passes 1,463 tests in 42 suites with no failures or skips.

### BLE-005: Heart-rate sensors compete with trainers for the active BLE slot

- Status: FIXED
- Code: `src/Train/AddDeviceWizard.cpp`, `src/Train/BluetoothDeviceTypes.h`,
  `src/Train/TrainingDeviceSelection.h`, `src/Train/TrainSidebar.cpp`,
  `src/Train/BT40Controller.cpp`, `src/Train/BT40Device.cpp`
- Impact: A separately configured Bluetooth heart-rate sensor is presented as
  another trainer. In the normal single-device Train view, selecting the KICKR
  excludes the Fenix and selecting the Fenix excludes the KICKR. Repeated
  disconnect/connect cycles can also delete a low-energy controller while Qt
  still reports `ClosingState`.
- Automated test: Selecting one trainer activates its configured Bluetooth
  heart-rate companion, assigns BPM to it, does not activate another trainer,
  and does not duplicate an explicitly selected heart-rate device. Lifecycle
  tests cover cancellation, ownership, repeated stop, active and closing link
  teardown, reconnect, late callbacks, and wizard cleanup.
- Resolution: A persisted `Bluetooth Heart Rate Sensor` type now uses the
  existing BLE controller but is activated automatically beside the selected
  trainer. Trainer-control writes are suppressed for that type. A closing
  `QLowEnergyController` is reparented and deleted after its disconnect signal
  instead of being destroyed synchronously in `ClosingState`.
- Verification: The device-selection suite passes 21 tests and the BLE lifecycle
  suite passes 17 tests, both normally and under strict ASan/UBSan/LSan. The
  full matrix passes 1,401 tests in 37 suites, the Qt 6.8.3 application starts,
  and simultaneous Bluetooth trainer plus heart-rate broadcast was verified on
  real hardware, including disconnect/connect recovery.

### DEV-002: ANT workers are not stopped and joined before destruction

- Status: FIXED
- Code: `src/ANT/ANT.cpp`, `src/ANT/ANTChannel.cpp`,
  `src/ANT/ANTlocalController.cpp`, `src/Train/AddDeviceWizard.cpp`,
  `src/Train/TrainSidebar.cpp`
- Impact: ANT workers can retain USB resources, race controller destruction, or
  trigger `QThread destroyed while running`.
- Regression test: `unittests/Train/antLifecycle` uses the production ANT worker
  and controller with a fake blocking transport. It covers direct worker and
  controller destruction, synchronous stop, immediate port reuse, repeated
  lifecycle operations, worker ownership, and channel ownership. The shared
  controller cleanup helper also has a focused unit test.
- Resolution: `ANT::stop()` now requests termination and synchronously joins
  the worker before returning. Controllers own their ANT worker and stop it in
  teardown; the Train sidebar explicitly disposes configured ANT controllers.
  Wizard cleanup uses the same stop/delete operation without arbitrary sleeps.
  ANT channels are QObject children of the worker, eliminating the channel and
  timer leaks exposed by the lifecycle sanitizer run.
- Verification: The dedicated RED suite observed a live worker, retained
  transport lease, failed immediate reuse, controller/worker ownership
  failures, and surviving channels before the fixes. It now passes 9 tests
  normally and under strict ASan/UBSan/LSan. The controller helper suite passes
  22 tests, the Qt 6.8.3 application builds and starts, and the complete matrix
  passes 1,473 tests in 43 suites with no failures or skips.

### DEV-003: ANT telemetry and command queues have data races

- Status: FIXED
- Code: `src/ANT/ANT.cpp`, `src/ANT/ANT.h`, `src/ANT/ANTChannel.cpp`,
  `src/ANT/ANTChannel.h`
- Impact: GUI and worker threads concurrently mutate queues and telemetry without
  a common lock, producing undefined behavior. Concurrent senders can also
  interleave an ANT frame with another frame before the required padding.
- Regression test: `unittests/Train/antThreadSafety` uses production ANT,
  ANTChannel, ANTMessage, RealtimeData, and CalibrationData code with a
  deterministic fake transport. It exercises every telemetry setter, races
  telemetry, requested controls, calibration, and channel enqueue/dequeue under
  TSAN, forces two senders to contend between an ANT frame and its padding, and
  records which thread performs setup, discovery follow-up, runtime control,
  timer, and shutdown I/O. It also races worker value publication against GUI
  reads and verifies that FE-C capability requests use the discovered channel.
- Resolution: Dedicated mutexes protect telemetry snapshots, requested
  control state, calibration state, channel command enqueue/dequeue, and complete
  frame-plus-padding transport transactions. A typed worker mailbox now owns
  setup, stop, load, gradient, mode, control-broadcast, and capability commands.
  Queue operations release their lock before channel or I/O work, and setup
  receives the startup acknowledgement on the worker-owned parser. Channel
  values use atomic publication, while discovery follow-up remains on the ANT
  worker; the queued GUI callback only publishes the discovered device. FE-C
  records the discovered channel before requesting capabilities.
- Verification: The first RED stage produced TSAN races in
  `getRealtimeData()` and `QQueue::dequeue()`, heap corruption in normal queue
  stress, and `frame, frame, padding, padding` on the fake wire. The second RED
  stage produced three deterministic wrong-thread I/O failures for setup,
  runtime/timer control, and shutdown; TSAN separately reported requested-mode
  publication and calibration reset/getter races. The final RED stage showed
  that discovery follow-up attempted I/O from the queued GUI callback, used FE-C
  channel 255 instead of the discovered channel, and raced worker value writes
  against `channelValue()` reads under TSAN. The resulting 13-test suite passes
  normally, under TSAN, and under strict ASan/UBSan/LSan. The complete matrix
  passes all 1,486 tests in 44 result blocks with no failures, skips, or
  sanitizer diagnostics.

### DEV-004: Stale ANT/BLE telemetry can be recorded indefinitely

- Status: FIXED
- Code: `src/ANT/ANTTelemetryFreshness.h`, `src/ANT/ANT.h`,
  `src/ANT/ANT.cpp`, `src/ANT/ANTChannel.h`,
  `src/ANT/ANTChannel.cpp`, `unittests/Train/antThreadSafety`
- Impact: Silent sensors retain their last values; some disconnect paths do not
  clear cadence. Recordings can contain plausible but stale data.
- Scope: BLE source ownership, stale fallback, and disconnect handling were
  already fixed by BLE-003. This change closes the remaining ANT path.
- Regression test: `unittests/Train/antThreadSafety` compiles the production
  ANT, ANTChannel, ANTMessage, and RealtimeData paths against a deterministic
  fake transport and a supplied monotonic time. It verifies initialized channel
  deadlines, deadline refresh on every telemetry frame, stale and lost
  source-specific clearing, fresh replacement ownership, primary-to-secondary
  cadence fallback, fast and slow metric expiry, and every telemetry setter.
- Resolution: Channel timing fields are initialized and every telemetry frame
  refreshes its blanking deadline. ANT telemetry now tracks the current source,
  priority, and monotonic publication time per metric. Fast telemetry expires
  after five seconds; temperature and core temperature expire after 30 seconds.
  Stale and lost channels clear only metrics they still own, while cumulative
  distance is retained.
- Verification: The first RED behavior tests left a refreshed channel blanked
  and retained a stale 153 bpm value. A compile-time RED stage then demonstrated
  that the required per-source freshness API did not exist, and a separate RED
  test exposed uninitialized channel timing state. The final 21-test suite
  passes normally, under strict ASan/UBSan/LSan, and under TSAN with QtTest's
  watchdog disabled to avoid its unrelated QWaitCondition teardown race. The
  complete matrix passes 1,523 tests in 45 suites with zero failures, skips, or
  blacklisted tests. The Qt 6.8.3 application builds and starts in an isolated
  offscreen smoke test.

### DEV-006: Windows ANT USB1 setup failures can hang shutdown

- Status: FIXED
- Code: `src/Train/USBXpress.cpp`, `src/Train/USBXpress.h`,
  `src/ANT/ANT.cpp`, `unittests/Train/usbXpressSafety`
- Impact: The USBXpress adapter reports a successful open after timeout or
  UART configuration failures. Because synchronous USBXpress I/O defaults to
  an infinite timeout, ANT can block in a stop-time write or its receive loop
  while `ANT::stop()` waits indefinitely. Normal shutdown also bypasses
  `SI_Close`, potentially retaining DLL or device state. Enumeration failures
  consume uninitialized outputs, VID/PID names are reversed twice, and short
  writes are reported as complete.
- Scope: Windows builds with both USBXpress and libusb enabled, using the
  Garmin USB1 stick (`0fcf:1004`).
- Regression test: `unittests/Train/usbXpressSafety` compiles the production
  Windows adapter and ANT transport against fake Windows, USBXpress, and libusb
  APIs on Linux. It data-drives every enumeration and post-open failure,
  requires rollback through exactly one `SI_Close`, verifies real VID/PID
  selection and actual transfer counts, and proves `ANT::closePort()` uses the
  USBXpress close API after the USB1 fallback opens.
- Resolution: VID and PID constants and product queries now use the canonical
  Garmin `0fcf:1004` identity. Enumeration and every setup result are checked;
  failed setup closes the opened handle and rejects the device. Successful
  opens install finite read and write timeouts. Reads and writes report actual
  transfer counts, close errors propagate, and ANT shutdown closes a USB1
  device through `SI_Close` instead of `CloseHandle`.
- Verification: Before the fix, the expanded harness reported 15 failures:
  swapped identity handling, accepted enumeration and setup failures, a short
  write reported as complete, ignored close errors, and the wrong ANT close
  API. The resulting 29-test suite passes normally and under strict
  ASan/UBSan/LSan. The Qt 6.8.3 application builds successfully, and the
  complete matrix passes all 1,515 tests in 45 suites with no failures, skips,
  blacklisted tests, or sanitizer diagnostics.

### THREAD-001: Cloud auto-download can outlive its athlete/context

- Status: FIXED
- Code: `src/Core/Athlete.cpp`, `src/Cloud/CloudService.cpp`,
  `src/Cloud/CloudService.h`, `unittests/Core/athleteMigrationSafety`
- Impact: Closing an athlete during download leaves an unowned worker that can
  dereference the destroyed athlete through a still-registered context. The
  worker's invalid-context guard also called `QThread::exit()` from an overridden
  `run()` method and then continued executing, because `exit()` only asks a
  thread event loop to stop.
- Regression test: `unittests/Core/athleteMigrationSafety` links the production
  `Athlete` and `CloudServiceAutoDownload` implementations to a blocking cloud
  provider. It requires an invalid-context worker to return promptly and closes
  an athlete while its real worker is waiting for a provider, requiring teardown
  to cancel, join, and delete the worker within two seconds.
- Resolution: Cloud auto-download now supports cooperative interruption. Its
  30-second provider wait and three-second completion delay use worker-local
  timers so cancellation wakes them promptly. Athlete teardown requests
  interruption, joins and deletes the worker before releasing the ride cache or
  athlete directories. Invalid contexts return from `run()` instead of calling
  the ineffective `exit()` method.
- Verification: Before the fix, the invalid-context test still had a running
  worker after three seconds and athlete teardown returned while its blocked
  cloud worker remained alive. The final 15-test suite passes normally, under
  strict ASan/UBSan/LSan, and under TSAN with no suppressions. The complete
  matrix passes 1,525 tests in 45 suites with zero failures, skips, or
  blacklisted tests. The Qt 6.8.3 application builds and reports its version
  successfully in an isolated offscreen smoke test.

### THREAD-002: Cloud download state is mutated from GUI and worker threads

- Status: FIXED
- Code: `src/Cloud/CloudService.cpp`, `src/Cloud/CloudService.h`,
  `src/Cloud/NetworkReplyWait.cpp`, `src/Cloud/NetworkReplyWait.h`,
  `src/Cloud/Nolio.cpp`, `src/Cloud/NolioTokenRefresh.cpp`,
  `src/Cloud/OAuthPKCE.cpp`, `src/Cloud/Strava.cpp`,
  `unittests/Core/athleteMigrationSafety`
- Impact: The GUI-affine QThread object and its worker path concurrently
  mutated provider, buffer, completion, and settings state. Provider callbacks
  could synchronously re-enter the next request, cancellation could destroy a
  provider during its active call, worker code wrote GUI-owned settings, and
  nested token refresh event loops had no timeout. Concurrent Nolio refreshes
  could also rotate the same refresh token more than once.
- Regression test: The production auto-download implementation is linked to
  controlled providers that complete inline, asynchronously, repeatedly, after
  timeout, and during cancellation or owner destruction. The suite verifies
  provider and buffer lifetime, stale-generation rejection, GUI event order,
  transactional settings conflict handling, empty/custom/default URL
  semantics across sequential saves, startup-sync payload validation, base
  network abort, SSL warning thread affinity, Nolio single-flight refresh and
  cache expiry, and network/OAuth timeout and interruption behavior.
- Resolution: A dedicated worker QObject owns providers and their buffers in
  the worker thread. It sends FIFO by-value events to the GUI, rejects stale
  generations, retires providers only after active callbacks return, and
  defers queue advancement until inline provider calls unwind. Settings are
  applied on the GUI thread as compare-and-swap transactions with chained
  baselines and effective default-URL canonicalization. Base cloud
  cancellation aborts child replies. Nolio refresh is a cancellable,
  one-minute cached single flight, while Nolio and OAuth waits share a bounded
  interruption-aware network helper.
- Verification: The added tests first exposed reentrant provider calls,
  retained buffers, duplicate completions, stale results, cross-thread settings
  writes, rejected URL payloads, repeated Nolio refreshes, and unbounded OAuth
  waits. The final focused suite passes all 56 tests normally, under strict
  ASan/UBSan/LSan, and under TSAN. TSAN ran with ASLR disabled and
  `ignore_noninstrumented_modules=1` for prebuilt Qt; no project suppression was
  used. The complete matrix passes 1,566 tests in 45 suites with zero failures,
  skips, or blacklisted tests. The Qt 6.8.3 application builds and reports
  `V3.8-DEV2605 (5012)` in an isolated offscreen smoke test.

### MEM-004: PythonDataSeries copy is a double-free/use-after-free

- Status: FIXED
- Code: `src/Python/SIP/Bindings.cpp`, `src/Python/SIP/Bindings.h`
- Impact: Generated copying shallow-copied the owning `double*`, so both
  wrappers later called `delete[]` on the same allocation.
- Test: Exercise copy and move construction and assignment, self-assignment,
  source destruction, and the SIP pointer-return bridge under ASan/UBSan.
- Resolution: `PythonDataSeries` implements deep-copy and noexcept move
  semantics with copy-swap assignment. The SIP pointer bridge explicitly
  adopts and destroys its heap wrapper while retaining `RideFile*` as borrowed.
- Verification: Normal and strict ASan/UBSan suites each pass 10 tests, both
  generated SIP ownership translation units and Python-enabled `Bindings.o`
  compile, and the aggregate suite passes 953 tests.

### MEM-005: PowerTap line reader can overflow its stack buffer

- Status: FIXED
- Code: `src/FileIO/PowerTapDevice.cpp`, `src/FileIO/PowerTapDevice.h`
- Impact: A device omitting CRLF could write beyond the 256-byte version
  buffer, while newline detection read one byte beyond initialized data.
- Test: Exercise null and zero buffers, 256-byte capacity boundaries, trailing
  CR, exact-capacity CRLF, timeout, read failure, and escaped binary context.
- Resolution: The line reader enforces its capacity before every byte read and
  scans CRLF only while both bytes are in range. Error context uses bounded
  `QString` construction and preserves the underlying serial error.
- Verification: Normal and strict ASan/UBSan suites each pass 11 tests,
  `PowerTapDevice.o` compiles, and the aggregate suite passes 964 tests.

### MEM-006: CAF parser relies on release-disabled bounds assertions

- Status: FIXED
- Code: `src/FileIO/TacxCafRideFile.cpp`
- Impact: Truncated and zero-record blocks caused out-of-bounds reads when
  release builds compiled out `Q_ASSERT`.
- Test: Truncate representative version 100 and 110 files at every byte, then
  exercise invalid counts, sizes, products, versions, and required blocks.
- Resolution: The importer decodes fixed-width little-endian fields only after
  validating the declared block count, per-block fingerprint and version,
  64-bit payload extent, required record dimensions, ordering, duplicates, and
  trailing data. Telemetry speed now comes from the stored `SpeedX10` field.
- Verification: The release and strict ASan/UBSan suites each pass 626 tests,
  the aggregate suite passes 943 tests, and `TacxCafRideFile.o` compiles in an
  isolated Qt 6.8.3 production build.

### MEM-007: TTS handlers accept empty/short blocks and unsafe typed reads

- Status: FIXED
- Code: `src/FileIO/TTSReader.cpp:528`,
  `src/FileIO/TTSReader.cpp:1042`, `src/FileIO/TTSReader.cpp:1141`
- Impact: Empty blocks can be read at negative offsets and short blocks access
  missing bytes. Typed pointer loads are also unaligned/aliasing unsafe.
- Resolution: The parser is transactional, validates every header and known
  record shape before allocation or reading, uses little-endian helpers for
  unaligned fields, decrypts payloads in place, and bounds individual payloads,
  decoded records, UTF-16 strings, block count, and cumulative working memory.
- Verification: The focused normal and ASan/UBSan suites each pass 178 tests,
  including all 0-15 byte inputs, truncations, integer overflow, memory
  amplification, UTF-16 replacement, and encryption-key wrapping. The
  aggregate suite passes 1,157 tests and the production `TTSReader.o` compiles.

### MEM-008: Custom virtual trainer names use mismatched new[]/delete

- Status: FIXED (`delete[]` paired with both `new char[]` allocation paths;
  parser and direct-add ownership tests pass under ASan/UBSan)
- Code: `src/Train/RealtimeController.cpp:716`,
  `src/Train/AddDeviceWizard.cpp:1275`,
  `src/Train/RealtimeController.cpp:745`
- Impact: Destroying a custom trainer invokes undefined allocator behavior.
- Test: Covered by `unittests/Train/virtualPowerTrainerOwnership` for both
  creation paths and repeated destruction.
- Fix: Pair the existing array allocations with `delete[]`.

### MEM-009: Cancelled migration leaves Athlete owning pointers indeterminate

- Status: FIXED
- Code: `src/Core/GcUpgrade.cpp`, `src/Core/Athlete.cpp`,
  `src/Core/Athlete.h`
- Impact: Constructor return after cancellation preceded member initialization,
  but the destructor later deleted those members.
- Resolution: All owned and borrowed pointers have deterministic defaults.
  `executeAfterConfirmation` centralizes the pre-construction gate used by
  startup and tab opening. `Athlete::createInNewContext` keeps a new Context
  owned until Athlete construction succeeds and rolls back its published UI
  state on failure. A construction scope guard releases partially initialized
  Athlete resources and clears the Context pointer.
- Verification: `unittests/Core/athleteMigrationSafety` compiles the production
  Athlete and GcUpgrade implementations and covers folder rejection, both
  compatibility-dialog outcomes, all-accepted execution exactly once, current
  and new users, construction from nonzero seeded storage, early construction
  failure, and late failure after cloud and other owners exist. The focused
  normal and ASan/UBSan/LSan runs pass 12 tests. The focused sanitizer harness
  excludes vptr because whole-translation-unit instrumentation requires RTTI
  from unrelated application classes; it constructs no fake polymorphic
  objects.

### MEM-010: RideFile leaks its four summary points

- Status: FIXED (all constructors and destruction pass under ASan/UBSan with
  leak detection enabled)
- Code: `src/FileIO/RideFile.cpp:69`, `src/FileIO/RideFile.cpp:85`,
  `src/FileIO/RideFile.cpp:106`, `src/FileIO/RideFile.cpp:118`
- Impact: Every parsed or temporary activity permanently leaks its minimum,
  maximum, average, and total `RideFilePoint` allocations.
- Test: `unittests/FileIO/rideFileOwnership` constructs and destroys every
  production constructor repeatedly. Before the fix LSan reported 384 leaked
  allocations (150,528 bytes); after the fix the same test is clean.
- Fix: Delete the four exclusively owned summary points in `RideFile::~RideFile`.

### MEM-011: Athlete leaks its directory structure

- Status: FIXED
- Code: `src/Core/Athlete.cpp`
- Impact: Every opened athlete leaked its owned `AthleteDirectoryStructure`
  and the internal directory strings retained by that object.
- Resolution: Athlete teardown deletes the directory structure after its
  synchronously owned path users have been destroyed. THREAD-001 subsequently
  added mandatory cloud-worker cancellation and joining before these directories
  are released.
- Verification: The MEM-009 real-lifecycle test first failed strict LSan with
  two leaked directory owners and now destroys the same production objects
  without leaks.

### MEM-012: RideFile leaks and aliases reference points

- Status: FIXED
- Code: `src/FileIO/RideFile.cpp`
- Impact: Every parsed reference or exhaustion point leaked at destruction and
  removal. The pointer-copying constructor also aliased the source's points, so
  adding correct destruction without a deep copy would introduce double frees.
- Test: `unittests/FileIO/rideFileOwnership` verifies independent copy ownership,
  source destruction, both removal paths, and final teardown. The original
  strict LeakSanitizer run reported five leaked points (1,960 bytes).
- Fix: Deep-copy reference points and delete them on removal and destruction.
- Verification: The focused normal and strict ASan/UBSan/LSan runs pass 5 tests.

### MEM-013: GSettings leaks its owned settings objects

- Status: FIXED
- Code: `src/Core/Settings.cpp`, `src/Core/Settings.h`
- Impact: Every destroyed `GSettings` instance leaked its global settings
  vector, contained `QSettings` objects, athlete wrappers, and their four
  athlete-specific `QSettings` objects. Reinitializing global/athlete settings
  also discarded the same owners without deleting them.
- Test: The real `GSettings` credential-routing and migration tests repeatedly
  construct, initialize, clear, and destroy both legacy and new-format settings
  under LeakSanitizer.
- Resolution: All pointers now have deterministic null initialization.
  `AthleteQSettings` owns and deletes its four settings objects, while
  `GSettings` deletes global and athlete collections both during clearing and
  final teardown. Both owner types are non-copyable.
- Verification: Before the fix strict LSan reported 39 allocations totaling
  4,402 bytes. The final 69-test ASan/UBSan/LSan credential suite exits cleanly
  with leak detection enabled, and the complete release matrix passes 1,930
  tests.

### MEM-014: DataFilter evaluates uninitialized and out-of-range vector data

- Status: FIXED
- Code: `src/Core/DataFilter.cpp`, `src/Core/DataFilterSafety.cpp`,
  `src/Core/DataFilterSafety.h`, and
  `unittests/Core/dataFilterSafety/`
- Impact: `estimates()` appended uninitialized `v1`/`v2` stack values
  when no power-duration model or parameter branch matched. Indexed vector
  assignment wrapped only when `rindex > count`, so assigning a non-empty
  right-hand vector to more selected indexes than it contained read
  `vector[count]` at the exact boundary. Negative, non-finite, or
  out-of-range selected indexes also reached an undefined floating-to-integer
  conversion, an overflowing resize, or an out-of-bounds write. User formulas
  could produce corrupted results or crash the process.
- Test: The new focused suite covers missing models, unknown parameters, every
  supported estimate field, duration estimates, exact vector-wrap boundaries,
  and negative, NaN, infinite, and overflowing target indexes. The test was
  compile-RED before the production helper existed.
- Resolution: Estimate pairs are appended only after a model or supported
  parameter matches. Repeated RHS values now use modulo indexing, and selected
  indexes must be finite, non-negative, and leave room for `index + 1`
  before any resize or write.
- Verification: The focused suite passes 11/11 normally and under strict
  ASan/UBSan with leak detection. The Qt 6.8.3 production application builds,
  and the complete release matrix passes 1,941 tests in 53 QtTest suites with
  zero failures, skips, blacklisted tests, or sanitizer/error markers.

### MEM-015: RideFile leaks and aliases its interval records

- Status: FIXED
- Code: `src/FileIO/RideFile.cpp`, `src/FileIO/RideFile.h`,
  `unittests/FileIO/rideFileOwnership/testRideFileOwnership.cpp`, and
  `unittests/Gui/splitRideData/testSplitRideData.cpp`
- Impact: Every imported, rebuilt, removed, or temporary interval leaked. The
  `RideFile` pointer-copying constructor also aliased interval records with the
  source, so adding destruction without first changing the copy semantics would
  cause double frees and cross-copy mutations. `RideFileInterval` additionally
  declared an unintended, uninitialized enum instance.
- Test-first evidence: Interval destructor, independent-copy, clear, rebuild,
  and removal cases were added before the ownership fix. The RED sanitizer run
  failed pointer independence and LeakSanitizer reported 241 allocations
  totaling 22,160 bytes. UBSan then exposed the unintended enum instance with
  the `0xBEBEBEBE` uninitialized-memory pattern.
- Resolution: `RideFile` deep-copies interval values, owns and deletes them at
  destruction, clear, rebuild, and removal boundaries, and no longer declares
  the unused enum instance. The split-ride test now relies on ordinary
  `RideFile` ownership instead of a compensating custom deleter.
- Verification: `rideFileOwnership` passes 8/8 and `splitRideData` passes 13/13
  under strict ASan/UBSan/LSan with no report. The fresh `-O2` release build and
  all 65 unit-test suites pass 2,096 tests with zero failures, skips, or
  blacklisted tests.

### MEM-016: RideFile leaks and aliases its calibration records

- Status: FIXED
- Code: `src/FileIO/RideFile.cpp` and
  `unittests/FileIO/rideFileOwnership/testRideFileOwnership.cpp`
- Impact: Every imported calibration record leaked when its `RideFile` was
  destroyed. The pointer-copying constructor also aliased calibration records
  with the source, so enabling the commented-out destructor cleanup alone
  would have introduced double frees and dangling pointers.
- Test-first evidence: The complete JSON regression target passed all 126
  functional cases before this ownership fix, but strict LSan reported 4,092
  bytes in 124 allocations. A focused copy-lifetime case then failed pointer
  independence and leaked its 40-byte calibration record.
- Resolution: `RideFile(RideFile*)` deep-copies every non-null calibration,
  and the `RideFile` destructor deletes and clears its owned calibration
  list.
- Verification: `rideFileOwnership` passes 10/10 and
  `jsonImportIntegrity` passes 126/126 both normally and under strict
  ASan/UBSan/LSan, with no sanitizer report. A fresh `-O2` release build and
  all 67 unit-test suites pass 2,296 tests (0 failed, 0 skipped, 0
  blacklisted).

### MEM-017: Duplicate RideFile updates use a freed point

- Status: FIXED
- Code: `src/FileIO/RideFile.cpp`, `src/FileIO/RideFile.h`, and
  `unittests/FileIO/rideFileOwnership/testRideFileOwnership.cpp`
- Impact: When `appendOrUpdatePoint` merged data into an existing timestamp,
  it deleted the temporary point and then passed that freed pointer to the
  minimum, maximum, and average accumulators. FIT files that append heart-rate
  data in a chained segment exercised this heap-use-after-free. The same path
  also counted a replacement as an additional sample, corrupting averages.
- Test-first evidence: The focused regression first failed functionally because
  one stored 151 bpm sample was reported as 75.5 bpm. The strict sanitizer RED
  run then stopped at `RideFile::updateMin` with a heap-use-after-free whose
  allocation and deletion both came from `appendOrUpdatePoint`.
- Resolution: A duplicate timestamp now keeps using the point owned by
  `dataPoints_`. Its previous contribution is removed from the aggregate
  totals and the merged contribution is added without incrementing the sample
  count; temperature's independent count is adjusted consistently.
- Verification: `rideFileOwnership` passes 11/11 normally and under strict
  ASan/UBSan/LSan. The production FIT reader integration passes 8/8 under the
  same sanitizer settings, including a real three-segment Garmin HRM Swim FIT
  file, with no sanitizer report.

### MEM-018: GPS smoothing loses point indexes and reads compacted output out of bounds

- Status: FIXED
- Code: `src/FileIO/FixGPS.cpp` (`GatherForAltitudeSmoothing`,
  `GatherForRouteSmoothing`, `FixGPSConfig::testClicked`, and
  `FixGPS::postProcess`), `src/FileIO/FixGPSSmoothingSafety.h`, and
  `unittests/FileIO/fixGpsSmoothingSafety/`
- Impact: The gather functions skip unreasonable locations but retain no map
  from each compacted spline control to its source ride point. Both apply paths
  subsequently index the compacted altitude or route output once for every
  original ride point. A ride containing an invalid location can therefore
  read past the output vector or write smoothed data onto the wrong point.
  Route gathering also reads a compacted smoothed-altitude entry before it
  rejects the current location, so trailing invalid points can overrun that
  input. Empty filtered control sets can be reported as successfully smoothed
  and are then indexed by the preview or apply path.
- Test-first evidence: The focused target first failed to compile because the
  production safety header did not exist. Its completed ten cases filter
  leading, middle, and trailing source entries, reject an all-filtered input,
  preserve indexes through a second mapped filter, reject short, duplicate,
  descending, out-of-range, and cardinality-mismatched maps before any write,
  and require at least four controls for a cubic spline.
- Resolution: Altitude and route controls now carry strictly increasing source
  indexes through preview, both spline passes, and application. Smoothed
  altitudes can feed route controls only through a validated aligned map, and
  output is applied only after its complete cardinality and index map pass.
  Empty and sub-cubic inputs fail before `size() - 1`, division, or endpoint
  access. B-spline evaluation allocates the library's full four-entry jet
  rather than a two-entry array that violated its documented output contract.
- Verification: All 10 focused cases pass normally and under strict
  ASan/UBSan/LSan with no sanitizer report. A fresh GCC 13 `-O2` production
  build emits no `FixGPS` or B-spline array-bounds warning, and the complete
  release matrix passes 71 suites and 2,441 tests (0 failed, 0 skipped, 0
  blacklisted).

### THREAD-003: Python chart execution races GUI object lifetime

- Status: FIXED
- Code: `src/Charts/PythonChart.cpp`, `src/Python/PythonEmbed.cpp`,
  `src/Python/PythonChartOwner.cpp`, `src/Python/PythonChartRunner.cpp`,
  `src/Python/PythonChartRunState.cpp`, `src/Python/PythonExecutionGate.cpp`,
  and `src/Python/SIP/Bindings.cpp`
- Impact: Worker code dereferenced GUI objects and a raw chart pointer while a
  nested GUI event loop allowed the chart to be edited or destroyed. Shared
  interpreter result, output, context, cancellation, and chart state also let
  concurrent callers overwrite one another.
- Resolution: Python chart inputs and filters are snapshotted as values on the
  GUI thread. An owned asynchronous runner coalesces reruns, rejects stale
  results, buffers chart commands for GUI-thread application, and cancels and
  joins its worker before UI teardown. A process-wide execution gate serializes
  complete interpreter runs, while unique run tokens make cancellation target
  the exact active run. Other Python callers now consume per-run results rather
  than shared mutable fields.
- Verification: `unittests/Python/pythonChartLifecycle` uses the production
  owner, runner, state, and gate. It deterministically covers source and filter
  snapshots, latest-only reruns, clear and repeated cancellation, stale-result
  suppression, GUI-thread callbacks, destructor cancellation and joining, gate
  waiter cancellation and serialization, and exact tokens. Its 13 QtTest cases
  pass normally and under ASan/UBSan/LSan and TSan. The Python-enabled
  application compiles, links, and passes its version smoke test. All 44 active
  unit-test targets pass; the legacy `seasonParser` fixture must be staged in
  that test's build working directory.
- Residual risk: An in-process native extension that permanently retains the
  GIL or deliberately ignores asynchronous interruption can still make joining
  unbounded. A hard deadline for such code requires process isolation rather
  than unsafe thread termination. The broader lifetime of raw `Context`,
  `RideItem`, and related pointers in `ScriptContext` remains a separate audit
  concern outside this fix.

### THREAD-004: Non-cooperative cloud provider calls can still block teardown

- Status: FIXED
- Code: `src/Cloud/CloudService.cpp`, `src/Cloud/CloudService.h`,
  `src/Cloud/LocalFileStore.cpp`, `src/Cloud/LocalFileStoreProcess.cpp`,
  `src/Cloud/MeasuresDownload.cpp`, `src/Cloud/WithingsDownload.cpp`,
  `src/Cloud/TredictMeasuresDownload.cpp`, and `src/Cloud/OAuthPKCE.cpp`
- Impact: `cancelAndWait()` must join the worker before athlete-owned paths are
  released. A provider stuck inside a synchronous syscall could therefore
  block athlete close forever.
- Resolution: Startup activity providers now fail closed unless they explicitly
  declare a cooperative or process-isolated execution contract. Unsupported,
  unclassified, and upload-only providers are skipped. The Local Store backend
  runs startup open, list, and read operations through a bounded helper process
  with framed input, root confinement, interruption handling, and a dedicated
  reaper that owns failed termination. Reaper admission closes atomically,
  pending registrations are retried, and shutdown reaches `Stopped` only after
  all helpers are drained. Non-Unix Local Store startup sync is disabled rather
  than falling back to an unbounded in-process call. Startup measures are
  restricted to Withings and Tredict, whose token and measure requests now have
  hard deadlines, interruption handling, and guarded athlete ownership.
- Verification: The new reaper regressions first failed because a quarantined
  process was not registered again (child exit 33), a failed shutdown was
  incorrectly marked stopped (41), and a queued dispatch could lose its worker
  target during thread exit (52). The final focused suite passes all 92 cases
  normally and under ASan/UBSan/LSan. Six reaper lifecycle cases pass under
  TSan without suppressions. The Qt 6.8.3 application compiles and links, and
  the complete qmake check run passes 1,615 tests in 46 QtTest suites with no
  failures or skips.

### SEC-004: OpenData discovery can redirect the full dataset

- Status: FIXED
- Code: `src/Cloud/OpenDataEndpointPolicy.h`,
  `src/Cloud/OpenDataEndpointPolicy.cpp`, `src/Cloud/OpenData.cpp`
- Impact: An HTTP-discovered arbitrary URL could receive the opted-in athlete
  UUID and complete activity dataset.
- Resolution: Discovery now uses a pinned HTTPS URL and is advisory only: it
  cannot add trust. Server roots must match the compiled HTTPS host, port, and
  root path exactly, with no userinfo, query, or fragment. Metrics URLs are
  built only from validated roots. Discovery, ping, and upload requests use
  manual redirect handling, and only non-redirected 2xx responses succeed.
  Requests abort after 30 seconds without transferred bytes. Discovery size
  and server count are bounded.
- Verification: The test-first target initially failed because the required
  endpoint-policy API did not exist while production still accepted arbitrary
  discovery strings. The final policy suite passes 26 cases normally and under
  ASan/UBSan/LSan, including attacker-only and mixed discovery responses,
  cleartext/lookalike/wrong-port/path/userinfo URLs, malformed and oversized
  responses, excessive server counts, and redirect rejection. The release
  application links and the full check passes 1,641 tests in 47 QtTest suites.

### SEC-005: Local HTTP API has no authentication or Host validation

- Status: FIXED
- Code: `src/Core/LocalApiSecurityPolicy.h`,
  `src/Core/LocalApiSecurityPolicy.cpp`, `src/Core/APIWebService.cpp`,
  `src/Core/main.cpp`
- Impact: When enabled, DNS rebinding could expose demographics, measures,
  zones, activities, and GPS data to a malicious website.
- Resolution: The API now generates a 256-bit base64url bearer token, persists
  it in `httpserver.ini`, and restricts that file to the owner on Unix.
  Invalid legacy tokens are rotated. New settings files are atomically
  published without direct-write fallback and with owner-only permissions.
  Non-regular and symbolic-link settings paths, including dangling links, are
  rejected before any write. Startup forces the configured host to
  `127.0.0.1`, verifies that the actual listener is loopback-only, and
  requires it to use the configured port. Every request
  must have exactly one valid loopback Host and one matching bearer
  Authorization header. Optional Origin headers must identify the same
  loopback API port. Missing, malformed, attacker-controlled, and duplicate
  security headers fail before athlete data is accessed. Token comparison is
  constant-time for equal-size tokens. The command-line server reports only
  the token file location, never the token, and the REST API documentation
  describes the new requirement.
- Verification: The test-first target initially failed because the security
  policy API did not exist. A second regression test reproduced the legacy
  malformed-INI startup failure before its compatibility fix. A third RED
  case showed configuration preparation could create a dangling symlink
  target. Extending it to startup initialization then failed to compile until
  the atomic initialization policy existed. The final suite passes all 44
  cases both normally and under ASan/UBSan/LSan. An isolated end-to-end server
  test returns 401 for missing and wrong tokens, 403 for attacker Host and
  Origin headers, and 200 for authenticated loopback CLI and browser requests.
  A process-level startup case rejects a dangling settings symlink with exit 1,
  preserves the link, and does not create its target. E2E also verifies `0600`
  settings permissions, forced loopback host, and that the token is absent
  from logs. The release application links, and the full check passes 1,685
  tests in 48 QtTest suites.

### SEC-006: Legacy OAuth callbacks are not bound to the initiating session

- Status: FIXED
- Code: `src/Cloud/OAuthCallbackPolicy.h`,
  `src/Cloud/OAuthCallbackPolicy.cpp`, `src/Cloud/OAuthDialog.cpp`,
  `src/Cloud/OAuthPKCE.cpp`
- Impact: HTTP callbacks, absent/fixed state, broad URL matching, and accepting
  TLS-handshake failure allow code interception or account-binding CSRF.
- Resolution: Interactive OAuth sessions now use independently generated
  256-bit base64url state values. A callback session accepts exactly one parsed
  callback whose scheme, host, port, encoded path, and constant-time-compared
  state match the initiating request. Missing, duplicate, malformed, mixed
  code/error, userinfo, fragment, wrong-origin, wrong-path, and replayed
  callbacks fail before a token request. Remote redirects require HTTPS; HTTP
  is accepted only for exact loopback redirects. Authorization and token
  endpoints must use HTTPS. TLS, network, malformed-JSON, non-object JSON, and
  empty-token failures now reject authorization before any credential setting
  is changed. Credential-only Xert and RideWithGPS grants use a private
  application URL instead of a synthetic HTTP callback. The reusable PKCE
  client now combines the system browser, random one-time state, exact
  loopback callback parsing, PKCE S256, replay rejection, and HTTPS endpoint
  validation.
- Verification: The test-first target initially failed to compile because the
  callback policy did not exist. Its final 46 cases pass normally and under
  ASan/UBSan/LSan. They cover random-state generation, exact HTTPS and
  loopback callbacks, HTTP downgrade, lookalike and wrong hosts, wrong path
  and port, userinfo, fragments, missing and duplicate parameters,
  authorization errors, replay, and TLS-handshake failure. The release
  application compiles and links, the OAuthPKCE-dependent migration suite
  passes 92 cases, and the complete qmake check passes 1,731 tests in 49
  QtTest suites. Live sign-in remains a manual compatibility check because
  provider-side redirect registrations and accounts are not available to the
  automated test environment.

### SEC-007: Active legacy providers send credentials and health data over HTTP

- Status: FIXED
- Code: `src/Cloud/CloudService.h`,
  `src/Cloud/SportsPlusHealth.cpp`,
  `src/Cloud/TrainingsTageBuch.cpp`, `src/src.pro`
- Impact: Network observers can capture credentials and full activity uploads.
- Test: Verify the discontinued providers cannot be registered, advertise no
  capabilities or credential settings, and reject both opening and activity
  uploads without changing the payload or emitting a completion signal.
- Resolution: SportPlusHealth and Trainingstagebuch are no longer compiled
  into the application or registered in the cloud-service UI. The factory
  rejects both legacy IDs to prevent accidental reintroduction. Their retained
  compatibility classes contain no endpoint, credential, parser, or network
  code; they expose zero capabilities and fail closed if directly instantiated.
  This disables the obsolete integrations instead of redirecting private data
  to an unverified replacement endpoint.
- Rationale: On 2026-07-09 the former SportPlusHealth API redirected to a
  parked-domain sales page, while Trainingstagebuch stated that its service
  closed on 2026-05-01 and its user data was deleted. Neither service offered
  a supported HTTPS API target suitable for migration.
- Verification: The test-first cases initially failed because SportPlusHealth
  was still registered and exposed `UserPass | Upload` capabilities. The
  final three regression cases pass normally and under strict
  ASan/UBSan/LSan. The complete `athleteMigrationSafety` suite passes 95
  tests, the release application compiles and links without either provider,
  and the complete qmake check passes 1,734 tests in 49 QtTest suites.

### SEC-008: Remote WebEngine downloads are automatically imported

- Status: FIXED
- Code: `src/Train/WebDownloadImportPolicy.cpp`,
  `src/Train/WebDownloadImportPolicy.h`, `src/Train/WebPageWindow.cpp`
- Impact: A malicious page using the shared profile can silently drive files
  into complex ride/workout parsers without a trusted origin or user gesture.
- Test: `unittests/Train/webDownloadImportPolicy` covers foreign and missing
  page identities, hidden pages, save-page downloads, declared and final size
  limits, insecure and deceptive URLs, prompt-time request mutation,
  concurrent requests, staging containment, cancellation, completion replay,
  path substitution, missing and empty files, symlinks, and safe filenames.
- Resolution: Every download is bound to the exact originating
  `QWebEnginePage`; other pages sharing the profile ignore it, while malformed
  owning-page requests fail closed. Remote page and download URLs require
  HTTPS, with cleartext HTTP restricted to exact loopback hosts. A plain-text,
  default-No prompt is required before acceptance. Approved files use a
  private random temporary directory and a generic sanitized filename. Only
  one request per page gate can be active, and completion is single-use and
  revalidates the page, exact path, canonical parent, regular-file type,
  symlink status, nonempty content, and a 128 MiB limit before invoking a
  parser. Closing the request or owner removes the staging directory and
  cancels unfinished state.
- Verification: The test-first build initially failed because no policy
  contract existed; focused edge cases then demonstrated unsafe size changes,
  cleartext remote URLs, missing identities, concurrent requests, and local
  file relationships before their fixes. All 52 policy tests pass normally
  and under strict ASan/UBSan/LSan. The release application compiles and
  links, the signal-lifetime scanner passes, and the complete qmake check
  passes 1,786 tests in 50 QtTest suites without failures or skips.
- Scope: Decompression expansion limits remain tracked separately as
  `PARSE-001`.

### SEC-009: Map WebChannel is exposed to insecure/untrusted scripts

- Status: FIXED
- Code: `src/Charts/MapPageSecurityPolicy.cpp`,
  `src/Charts/MapPageSecurityPolicy.h`,
  `src/Charts/RideMapWindow.cpp`, `src/Charts/RideMapWindow.h`,
  `src/Resources/map.qrc`
- Impact: Google Maps JavaScript is loaded over HTTP in a page exposing route
  coordinates and interval mutation through WebChannel.
- Test: `unittests/Charts/mapPageSecurity` covers unsafe map types, tile
  templates and image origins, cleartext and deceptive URLs, exact qrc
  resource admission, main-frame navigation, single-use `setHtml()`
  authorization, CSP nonce validation, JavaScript string encoding, and the
  bundled Leaflet asset hashes.
- Resolution: The map now uses a dedicated off-the-record WebEngine profile
  with memory-only caching, no persistent cookies or permissions, and no
  shared browser state. Leaflet 1.9.4 and its license/assets are bundled and
  hash-pinned; the legacy Google option and remote script load are removed,
  with old saved map values falling back to OpenStreetMap. An interceptor
  admits only exact qrc page/script/style assets and validated tile image
  origins. Remote tile templates require HTTPS; HTTP is limited to exact
  loopback hosts. A nonce-based CSP denies all other script, connection,
  frame, worker, object, base, and form sources. Navigation, new windows,
  file selection, JavaScript dialogs, context menus, drops, and downloads are
  blocked on the privileged page. Qt's internal `setHtml()` `data:` transport
  is admitted by the request interceptor but accepted by the page only through
  a single-use authorization that is consumed by any attempted navigation.
- Verification: Test-first cases failed on the missing policy, remote HTTP
  and script paths, unsafe legacy map selection, broad qrc URLs, and
  data/about main-frame navigation before their fixes. A release usage test
  then exposed that rejecting all `data:` requests also rejected Qt's trusted
  `setHtml()` transport; the follow-up RED test required a missing single-use
  navigation gate. All 75 focused tests pass normally and under strict
  ASan/UBSan/LSan. The release application compiles and links, and the complete
  qmake check passes 1,861 tests in 51 QtTest suites without failures or skips.
  A usage test opened an isolated copy of an existing athlete profile, selected
  a GPS activity, and rendered both OpenStreetMap tiles and the route
  successfully through the gated path.

### SEC-010: Interval names are inserted into JavaScript without escaping

- Status: FIXED
- Code: `src/Charts/MapPageSecurityPolicy.cpp`,
  `src/Charts/RideMapWindow.cpp`
- Impact: An imported activity can provide an interval name that breaks out of a
  JavaScript string, reads route data through WebChannel, and sends it remotely.
- Test: The map policy suite round-trips quotes, slashes, newlines, NUL, Unicode
  line separators, HTML metacharacters, and closing-script payloads through a
  JSON parser and asserts that raw script-breaking characters are absent.
- Resolution: Dynamic JavaScript strings are serialized through Qt's JSON
  encoder, with HTML-breaking characters and JavaScript line separators
  escaped. Interval tooltips/titles and tile templates no longer interpolate
  user-controlled text into quoted JavaScript source.
- Verification: All encoding cases and the production integration contract
  pass normally and under strict ASan/UBSan/LSan, as well as in the complete
  1,861-test regression run.

### SEC-011: Cloud credentials are stored in plaintext settings

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialStoreQtKeychain.cpp`, `src/Core/Settings.cpp`,
  `contrib/qtkeychain`, and `.devcontainer/package-appimage.sh`
- Impact: Backups or local processes able to read athlete settings obtained
  reusable passwords, access tokens, and refresh tokens. Credential-bearing
  INI files were also left with process-default permissions.
- Resolution: All 29 configured credential keys are intercepted by
  `GSettings` and stored in the native OS credential vault through QtKeychain,
  with insecure plaintext fallback disabled. Vault identifiers combine a
  persistent random global/athlete scope with an opaque credential-key hash.
  Existing plaintext values migrate only after a confirmed vault write, and
  failed writes remain memory-only while any previous plaintext source is
  retained and restricted to mode 0600. Scope creation fails closed unless its
  identifier is persisted. Scope mappings survive both pre- and
  post-initialization fallback paths. Failed deletion creates a non-secret
  tombstone so an old vault value cannot reappear on a later launch. Cached
  success, failure, and absence states do not hide duplicate or later legacy
  sources. AppImage packaging explicitly includes libsecret and both third-party
  license files.
- Test: `unittests/Core/credentialSettings` covers every allowlisted key,
  native-backend status mapping, disabled insecure fallback, vault precedence,
  new writes, migration, replacement, deletion retry, duplicate sources,
  transient failures, stable and unwritable scopes, legacy/new settings
  routing, and both scope initialization orders. The follow-up tests first
  reproduced loss of old-format credentials, failed replacement, duplicate
  plaintext retention, negative-cache masking, deletion resurrection,
  unpersisted scope IDs, transient empty-value masking, and both scope-order
  discontinuities before their fixes.
- Verification: The focused suite passes all 69 tests normally and under strict
  ASan/UBSan/LSan with leak detection. The complete release matrix passes 1,930
  tests in 52 QtTest suites with zero failures, skips, blacklisted tests, or
  sanitizer/error markers. The Qt 6.8.3 production application links and the
  166,402,552-byte AppImage reports `V3.8-DEV2605 (5012)`; its bundled
  libsecret dependency chain resolves and its QtKeychain/libsecret licenses are
  present. Two isolated, network-disabled AppImage launches without a session
  keyring retried both sentinel migrations on the next process start, retained
  both values, and hardened the private INI file to mode 0600.

### SEC-012: Secrets are logged or placed in URLs

- Status: FIXED
- Code: `src/Cloud/CloudCredentialTransport.{h,cpp}`,
  `src/Cloud/{Azum,OAuthDialog,RideWithGPS,WithingsDownload}.cpp`, and
  `unittests/Cloud/credentialTransportSafety`
- Impact: Tokens/passwords can enter terminals, journald, crash reports, proxy
  logs, history, or support bundles.
- Resolution: Removed credential values and raw token, upload, and health-data
  payloads from the affected provider diagnostics. Withings measure requests
  now send the access token only as an `Authorization: Bearer` header and the
  date range in a form-encoded POST body. Ride with GPS credential exchange now
  uses its documented API v1 token endpoint, API-key header, and JSON request
  body instead of the legacy credential-bearing URL; the response parser
  accepts the documented nested token and the legacy form for compatibility.
  Diagnostics on these paths now contain event metadata only.
- Test: `unittests/Cloud/credentialTransportSafety` first failed to build
  because the requested transport helper did not exist. Its sentinel tests now
  verify exact URLs, headers and bodies, reject every credential from URLs,
  cover documented and legacy Ride with GPS token responses, and guard the
  production call sites against the removed logging/query patterns. The
  existing `athleteMigrationSafety` server now waits for the complete HTTP
  body and proves the integrated Withings POST method, Bearer header, empty
  query, and encoded date parameters.
- Verification: The focused suite passes all 8 tests normally and under strict
  ASan/UBSan/LSan with leak detection; the focused Withings integration passes,
  and the Qt 6.8.3 release application compiles and links. Two complete release
  matrix runs pass 1,949 tests in 54 test projects with zero failures, skips, or
  blacklisted tests. The 166,402,552-byte AppImage reports
  `V3.8-DEV2605 (5012)`, has SHA-256
  `d359a6413c6aed1fdc7934960bb2fdbf675598e2c401ce43d9617c2a5309cecd`,
  and remained running for its full isolated 15-second GUI smoke test; its only
  log message was the pre-existing missing Finnish translator notice.

### GUI-004: Distance merge can index past the source activity

- Status: FIXED
- Code: `src/Gui/MergeActivityDistanceCursor.cpp:27`,
  `src/Gui/MergeActivityWizard.cpp:315`, `src/Gui/MergeActivityWizard.cpp:357`
- Impact: When the destination continues beyond the source's final distance, or
  the source is empty, the scan reaches `dataPoints().count()` and then indexes
  that element. Debug builds assert; release builds can crash or access invalid
  memory while combining activities.
- Test: Exercise exact and in-between destination distances, past-end targets,
  an empty source, and null source entries. Verify that the cursor stays
  exhausted and never exposes an invalid index.
- Resolution: A monotonic source cursor now returns either an in-range sample
  pointer or null. Distance merge skips interpolation and source-series copies
  when no source sample covers the destination distance.
- Verification: The regression project first failed in RED because the
  production cursor did not exist. The focused normal and strict ASan/UBSan/LSan
  suites each pass 6 tests. The Qt 6.8.3 release application builds and links
  both changed production objects. The complete matrix passes 1,971 tests
  across 56 projects with zero failures, skips, blacklists, or sanitizer reports.
  The packaged AppImage (`fde4ebafcbd8742e305c826615dcda04403132e8f9610b1506493af29b2aeb46`)
  also remained running for its full isolated 15-second X11 smoke test.

### GUI-005: Failed resampling is accepted and later dereferenced

- Status: FIXED
- Code: `src/Gui/MergeActivityRidePreparation.cpp:24`,
  `src/Gui/MergeActivityWizard.cpp:115`,
  `src/Gui/MergeActivityWizard.cpp:745`,
  `src/Gui/MergeActivityWizard.cpp:812`,
  `src/Gui/MergeActivityWizard.cpp:915`, `src/FileIO/RideFile.cpp:3268`
- Impact: `RideFile::resample()` can return null for short or unresampleable
  input, but source-selection pages still accept it and later dereference the
  missing working ride. A one-point activity with a different recording
  interval deterministically reaches this crash path.
- Test: A one-point, mismatched-interval source must fail preparation while
  retaining the previous working ride and without emitting its deletion
  signal. Successful preparation must replace the old ride, preserve samples,
  and deep-copy XData; a null source must keep the legacy clearing behavior.
- Resolution: Ride preparation now builds the resampled candidate under RAII,
  copies XData, and swaps it into the wizard only after all preparation
  succeeds. `setRide()` reports the result, and import, device download, and
  existing-activity selection all remain on their current page on failure.
  The resampler's null-return path also deletes every temporary spline.
- Verification: The regression project first failed in RED because the
  production preparation helper did not exist. The focused normal suite passes
  all 5 tests. Its first strict ASan/UBSan/LSan run exposed four leaked
  `SplineLookup` allocations on the same null-return path; after adding cleanup,
  the strict suite passes all 5 tests with leak detection and no sanitizer
  reports. The Qt 6.8.3 release application builds and links all changed
  production objects. The complete release matrix passes 1,976 tests across 57
  projects with zero failures, skips, or blacklisted tests. The packaged
  AppImage has SHA-256
  `40624c4534b5923331d1f31834ff6d25370c6af40cda78abba29c833a8343c54`
  and remained running for its full isolated 15-second X11 smoke test; its only
  log messages were the expected missing `C` translator notices.

### PERF-001: Merge activity alignment is O(series * samples^2) on the UI thread

- Status: FIXED
- Code: `src/Gui/MergeActivityAlignment.cpp`,
  `src/Gui/MergeActivityAlignment.h`,
  `src/Gui/MergeActivityWizard.cpp`
- Impact: Multi-hour activities could execute hundreds of millions of
  iterations while freezing the UI.
- Regression test: `unittests/Gui/mergeActivityAlignment` first failed to
  build because the requested alignment helper did not exist. It now compares
  short and FFT paths with the exact legacy scorer, covers the 512/513
  boundary, positive/negative offsets, over 64 tied periodic candidates,
  zero/constant legacy behavior, cooperative cancellation, worker lifecycle,
  and bounded one-hour/three-hour runtime and scaling.
- Fix: Snapshot shared series on the GUI thread, score short inputs exactly,
  and use GSL radix-2 convolution plus prefix sums for long inputs. A bounded
  exact-rescore set preserves legacy ordering while reducing normal work to
  `O(series * samples * log(samples))`. A cooperative `QtConcurrent` runner
  keeps the wizard responsive, disables conflicting navigation during work,
  and joins safely on cancellation or destruction.
- Verification: The focused suite passes all 16 tests normally and under
  strict ASan/UBSan/LSan with leak detection. The Qt 6.8.3 application compiles
  and links as a true `-O2`, `QT_NO_DEBUG` release; the complete offscreen
  matrix passes 1,965 tests in 55 projects with zero failures, skips, or
  blacklisted tests. The 166,431,224-byte AppImage reports
  `V3.8-DEV2605 (5012)`, has SHA-256
  `7fc991fbf1d9574a9670da5642c1109809cc036a77cd74eaf4b08e8923a0ccb8`,
  and remained running for its full isolated 15-second direct-launch X11 smoke
  test; its log contained only missing `C` translator notices.

### PERF-002: Bulk import parses files repeatedly and rebuilds global state

- Status: FIXED
- Code: `src/Gui/RideImportWizard.cpp`,
  `src/Gui/RideImportRideStore.h`, `src/Core/RideCache.cpp`,
  `src/Core/RideCacheBulkMerge.h`
- Impact: Imported files were parsed during validation and again during save.
  Every saved ride could also reset and sort the full activity model on the UI
  thread.
- Regression test: The RED build failed because the production batch helpers did
  not exist. The completed focused suite verifies one parse per successful or
  failed source, exact parser-call counts for 100- and 1,000-file batches,
  aligned ownership/error state, and bulk merges of 100 and 1,000 rides into a
  10,000-ride cache. It also verifies one model reset, bounded comparisons,
  sorted output, duplicate replacement, and an empty no-op merge.
- Resolution: Validation now retains each parsed `RideFile` through save,
  including multi-ride archives and failures. Successful imports are published
  as one cache batch with one merge/sort/model reset, one selection update, and
  one estimator refresh while preserving per-ride notifications and abort
  behavior.
- Verification: 11 focused tests passed in normal and strict
  ASan/UBSan/LSan builds with no sanitizer reports. A fresh release build and all
  58 unit-test projects passed (1,987 passed, 0 failed, 0 skipped, 0
  blacklisted). The packaged AppImage SHA-256 is
  `c2a3bc30a2d927e1181f12e3c554cf240218055dcbd8d41ed9cbc08d1c695560`;
  it remained running for its full isolated 15-second X11 smoke test, whose log
  contained only missing `C` translator notices.

### PERF-003: RideCache blocks startup and repeatedly scans the full library

- Status: FIXED
- Code: `src/Core/RideCache.cpp`, `src/Core/RideCache.h`,
  `src/Core/RideCacheModel.cpp`, `src/Core/RideDB.y`,
  `src/Core/RideCacheSnapshot.cpp`, and
  `src/Core/RideCacheStartup.h`
- Impact: Discovering and sorting every activity blocked athlete construction.
  Restoring the persisted cache then delayed all remaining startup work, while
  broad configuration changes synchronously cancelled refresh workers and
  rescanned the complete library.
- Regression test: The RED build failed because the production startup helper
  did not exist. The completed focused suite measures cold and warm indexing at
  1,000, 10,000, and 50,000 activities; verifies 512-item batches and a maximum
  of four queued snapshot batches; rejects stale snapshot targets; verifies
  dependency-specific invalidation; and exercises superseded and cancelled
  refresh generations.
- Resolution: File discovery, timestamp indexing, and persisted-cache parsing
  now run on a background loader. The model receives bounded incremental
  inserts, becomes interactive after the sorted file index is available, and
  receives move-only cache snapshots through a bounded GUI queue. Snapshots
  apply only to the same untouched activity. Refresh requests now coalesce by
  generation, interrupt superseded workers without blocking the GUI, and scan
  only for metric dependencies; cosmetic metadata and color changes no longer
  trigger a full metric refresh.
- Verification: 12 focused tests passed normally and under strict
  ASan/UBSan/LSan with no sanitizer reports. Fresh production and full release
  builds succeeded, and all 59 unit-test projects passed (1,999 passed, 0
  failed, 0 skipped, 0 blacklisted). The 166,451,704-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `ea734a57d2d96a61420710480ecf94301ecae3afa05e54a2e14a3381890b83cc`.
  It remained stable for a 20-second isolated X11 launch and a 45-second launch
  against a copied real athlete profile; neither run produced a crash, cache
  parser error, or Qt linkage error.

### PERF-004: DataFilter leaks models and its GSL RNG

- Status: FIXED
- Code: `src/Core/DataFilter.cpp`, `src/Core/DataFilter.h`, and
  `src/Core/DataFilterResources.h`
- Impact: Frequently recreated filters retain five model objects plus an RNG,
  increasing memory use during navigation.
- Regression test: The RED build failed because the production resource owner
  did not exist. The completed test creates and destroys 10,000 resource sets,
  each containing five counted models and one real GSL RNG, and verifies that
  all 50,000 models are destroyed. Strict leak detection covers the RNG.
- Resolution: `DataFilterResourceOwner` now owns the model objects and GSL RNG,
  deleting the models with `qDeleteAll` and releasing the RNG with
  `gsl_rng_free`. Both `DataFilter` constructors use one common initializer,
  and destruction clears the runtime's non-owning model aliases before the
  owner releases their targets.
- Verification: All three focused tests passed normally and under strict
  ASan/UBSan/LSan with leak detection and no sanitizer reports. A fresh full
  release build succeeded, and all 60 unit-test projects passed (2,002 passed,
  0 failed, 0 skipped, 0 blacklisted). The 166,455,800-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `ad6cba25536acbc84febfc0c160ca86d6eacc1e50befda7f87e6fc0c2fd35b42`.
  It remained stable for a 15-second isolated X11 launch and a 45-second launch
  against a copied real athlete profile; neither run produced a crash, cache
  error, sanitizer report, or Qt linkage error.

### PERF-005: Navigator filtering is quadratic for large result sets

- Status: FIXED
- Code: `src/Gui/RideNavigatorSearchFilter.h` and
  `unittests/Gui/rideNavigatorSearchFilter/testRideNavigatorSearchFilter.cpp`
- Impact: `filterAcceptsRow` performs a linear `QStringList::contains` lookup for
  every source row, making large search result sets quadratic.
- Regression test: The initial RED build failed because the production search
  filter had no independently testable header. After extracting the unchanged
  `QStringList` implementation, the functional cases passed but filtering
  50,000 rows against 25,000 matches took 3,173 ms and failed the 1,000 ms
  budget. The completed suite verifies exact case-sensitive membership,
  duplicate matches, search replacement and clearing, and combination with the
  planned/completed activity filter.
- Resolution: The actual navigator `SearchFilter` is now independently
  testable and stores the current filenames in a reserved `QSet<QString>`,
  replacing each per-row linear lookup with an amortized constant-time lookup
  while preserving the existing matching semantics.
- Verification: All five focused QtTest cases passed in 16 ms normally and in
  69 ms under strict ASan/UBSan/LSan, with no sanitizer reports. A fresh full
  release build succeeded, and all 61 unit-test projects passed (2,007 passed,
  0 failed, 0 skipped, 0 blacklisted). The 166,451,704-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `3302dabf6da1fed9488463306ef3f073b22eb09a104f9786673259637726f302`.
  It remained stable for a 15-second isolated X11 launch and a 45-second launch
  against a copied real athlete profile; the logs contained only missing
  translator notices.

### PERF-006: Calendar/compare aggregation repeatedly scans the library

- Status: FIXED
- Code: `src/Core/RideCacheAggregate.h`, `src/Core/RideCache.cpp`,
  `src/Charts/CalendarWindow.cpp`, `src/Gui/ComparePane.cpp`
- Impact: Each metric and time bucket triggers another full activity scan.
- RED test: The existing `rideCachePerformance` suite was extended before the
  implementation with batch semantic, relevance-union, and 50,000-row scaling
  cases. The test project referenced the not-yet-existing aggregate helper;
  qmake warned that the header was missing and compilation failed with exit 2.
- Regression coverage: The semantic case covers all existing aggregation
  modes, zero inclusion/exclusion, the temperature sentinel, disabled metrics,
  non-finite values, and the existing standard-deviation formatting behavior.
  The 50,000-row, 52-bucket, six-metric case has a two-second budget and proves
  exact callback counts: 2,600,000 specification checks and only 300,000 value
  and count reads, eliminating the previous metric multiplier from activity
  scans. Relevance tests preserve the union of multiple specifications.
- Resolution: `RideCache` now batches metrics and specifications through one
  generic traversal while retaining the single-metric compatibility wrappers.
  Calendar summaries submit every bucket in one request. Compare-season
  relevance and aggregation requests are grouped by their source cache, then
  mapped back to the existing table rows.
- Verification: All 15 focused QtTest cases passed in 80 ms normally and in
  303 ms under strict ASan/UBSan/LSan, with no sanitizer reports. A fresh full
  release build succeeded, and all 61 unit-test projects passed (2,010 passed,
  0 failed, 0 skipped, 0 blacklisted). The 166,480,376-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `6bc0afb5f0e4a5625afdba6c9b4bbfb71cdbd86810cfd135dd00993d13f7245e`.
  It remained stable for a 15-second isolated X11 launch and a 45-second launch
  against a copied real athlete profile; the logs contained only missing
  translator notices.

### PERF-007: Cloud GUI handoff queues have no backpressure

- Status: FIXED
- Code: `src/Cloud/CloudService.cpp`, `src/Cloud/CloudService.h`,
  `unittests/Core/athleteMigrationSafety/CloudAutoDownloadTestSupport.cpp`,
  `unittests/Core/athleteMigrationSafety/CloudAutoDownloadTestSupport.h`, and
  `unittests/Core/athleteMigrationSafety/testAthleteMigrationSafety.cpp`
- Impact: Auto-download result/progress events, settings transactions, and
  queued SSL notifications can accumulate without a count or byte limit while
  the GUI thread is blocked. A fast or faulty provider can grow memory use and
  leave a long stale-event tail after cancellation.
- RED test: Five stalled-GUI regression cases were added before the production
  implementation. A clean focused build compiled the tests and then failed at
  link time with exit 2 because the bounded-queue APIs and test probes did not
  exist. The missing symbols covered auto-download queue statistics and the
  settings/SSL handoff reset and statistics functions.
- Regression coverage: A 4,096-event progress flood proves an eight-snapshot
  maximum, one queued GUI dispatch, monotonic delivery, and an exact final
  100% state. Six 32 KiB payloads under a two-result limit prove producer
  blocking and exactly-once parsing, while cancellation of a blocked ten-file
  run completes in under two seconds and leaves no events, signals, imports,
  or worker buffers. A 4,096-update same-provider settings chain composes to
  one compare-and-set transaction with the final athlete and global values.
  Finally, 4,096 duplicate and 64 distinct SSL warnings remain within 32 items
  and 256 KiB, preserve every occurrence through counts or omissions, and
  produce one aggregate GUI-thread notification.
- Resolution: Auto-download now admits at most eight result payloads and
  128 MiB, retains at most eight progress snapshots, blocks result producers
  until the GUI releases credit, and purges stale generations while waking
  blocked producers during cancellation or restart. Payload credit remains
  charged through GUI parsing. Settings handoff is capped at 64 transactions
  and 1 MiB; compatible same-athlete/same-provider compare-and-set chains are
  composed without weakening their first expectation, and overflow fails
  closed without advancing the provider baseline. SSL handoff retains at most
  32 distinct warnings and 256 KiB, deduplicates exact messages with occurrence
  counts, records omissions, and keeps one GUI dispatch outstanding through
  the aggregate modal warning. SSL errors are still never ignored.
- Verification: All 100 focused QtTest cases passed normally in 9,794 ms and
  under strict ASan/UBSan/LSan in 13,112 ms, with no sanitizer reports. The five
  new concurrency cases also passed under TSan (seven QtTest results including
  initialization and cleanup) in 2,525 ms with no race report. A fresh full
  release build succeeded, and all 61 unit-test projects passed (2,015 passed,
  0 failed, 0 skipped, 0 blacklisted). The 166,496,760-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `a4ff9affc392e587f83b77a0d0a180cf32b62ad9190fb90836d401050097ed1a`.
  It remained stable for a 15-second isolated X11 launch and a 45-second launch
  against a copied real athlete profile; the logs contained only missing
  translator notices.

### CLOUD-003: Strava HTTP failures can be imported as successful activities

- Status: FIXED
- Code: `src/Cloud/Strava.cpp:576`, `src/Cloud/Strava.cpp:591`,
  `src/Cloud/Strava.cpp:662`, `src/Cloud/Strava.cpp:998`,
  `src/Cloud/StravaApiReplyPolicy.cpp:224`,
  `src/Cloud/StravaApiReplyPolicy.cpp:255`,
  `src/Cloud/StravaApiReplyPolicy.cpp:328`,
  `src/Cloud/CloudService.h:228`, and
  `src/Cloud/CloudService.cpp:2943`
- Impact: The activity completion slot neither checks the reply error nor the
  HTTP status before passing the payload to the activity parser and reporting
  `Completed`. A Strava 401/403 Fault object is valid JSON, so it is converted
  into an empty activity with an invalid start time and can be offered for
  import. A failed stream request is silently ignored, which can also make a
  superficially successful import omit heart rate and other samples.
- Evidence: `readFileCompleted()` unconditionally calls `prepareResponse()` and
  `notifyReadComplete()`. `prepareResponse()` accepts every JSON object without
  requiring activity identity or timestamps, while `addSamples()` returns
  silently on any network error. Strava documents 401 and 403 Fault responses
  for failed API requests.
- Test: Drive production activity and stream completion paths with controlled
  401, 403, malformed-2xx, and valid replies. No failed payload may be parsed,
  published, or reported complete; activity and stream failures must retain
  their actionable authentication or scope error.
- Fix direction: Validate transport, HTTP status, content type, JSON shape, and
  required activity fields before conversion. Propagate one bounded,
  credential-redacted provider error through the existing completion API and
  clean up every reply and buffer exactly once.
- Test-first evidence: The first policy build failed because the production
  reply validator did not exist. A controlled provider then failed to compile
  against the missing four-argument completion contract. After those RED
  cases, malformed location and numeric stream fixtures failed 2/11 because
  the initial validator accepted them, and a transport failure after HTTP 200
  failed separately because it was incorrectly described as `HTTP 200`.
- Resolution: Activity and stream replies are now bounded before buffering and
  validated for transport status, HTTP status, JSON content type, document
  shape, required activity identity and date, stream shape, sample type, and
  consistent sample count. Provider failures become bounded,
  credential-redacted failure completions; neither manual sync nor
  auto-download parses or imports those payloads. Both response layers remove
  their reply maps and schedule every reply for deletion. Stream conversion
  now iterates the validated sample count, so an empty stream list terminates
  without points and a normal stream no longer gains a trailing blank sample.
- Verification: All 34 focused policy cases pass normally and under
  ASan/UBSan/LSan. The full 103-case athlete migration and cloud-lifecycle
  suite passes both normally and under the same sanitizers, including exact
  buffer, request, and reply lifetime checks. The production application
  compiles and links with 0 errors and 0 warnings, and the complete release
  matrix passes 73 suites and 2,483 tests with no failures, skips, or
  blacklists.

### THREAD-007: Concurrent Strava refreshes race rotating refresh tokens

- Status: FIXED
- Code: `src/Cloud/Strava.cpp:147`,
  `src/Cloud/OAuthDialog.cpp:683`,
  `src/Cloud/StravaTokenRefresh.cpp:218`,
  `src/Cloud/StravaTokenPublication.cpp:31`, and
  `src/Cloud/StravaCredentialPublisher.cpp:66`
- Impact: Every `Strava::open()` refreshes from the instance's current token.
  Sync, upload, and auto-download can create independent provider clones and
  exchange the same refresh token concurrently. Strava invalidates an old
  refresh token as soon as a new one is returned, so one operation can fail or
  a later-finishing stale transaction can overwrite the newest credentials.
- Evidence: The refresh path has no process-wide single flight, generation
  check, or compare-and-swap result publication. The last-refresh setting is
  written but never consulted. The official Strava OAuth contract requires
  clients to retain and subsequently use the most recently returned refresh
  token.
- Test: Start two controlled `open()` calls with the same input token, complete
  their fake exchanges in reverse order, and force both success and failure
  outcomes. Exactly one exchange may run; every waiter must receive the same
  result, cancellation must not cancel the leader, and only the newest token
  pair may be persisted.
- Fix direction: Add an athlete/token-keyed cancellable single-flight refresh
  coordinator with a short successful-result cache. Publish credentials with a
  generation-aware transaction and do not report success until the current
  pair has been accepted for persistence.
- Test-first evidence: The first coordinator test failed to compile because no
  single-flight API existed. Follow-up RED tests reproduced same-account token
  rotation, stale snapshots, cache expiry, cancellation, invalidation, and a
  new OAuth grant superseding an active refresh. Separate storage tests first
  failed on missing checked writes, compare-and-swap publication, GUI-thread
  credential handoff, and the exact refresh token submitted to Strava. Finally,
  commit `caf59b2` ran against the old production wiring: 32 policy cases
  passed and the two new `Strava::open()`/OAuth integration contracts failed.
- Resolution: Refreshes now use one per-athlete single flight, a one-minute
  successful-result cache, a bounded set of rotating-token aliases, and a
  generation that invalidates stale work. Different athletes can still refresh
  concurrently. Results retain the exact refresh token submitted to Strava,
  and publication compares that token with the current durable value before
  writing refresh token, access token, and timestamp in order. Checked
  credential writes are marshalled to the settings thread with bounded,
  cancellable waits; a timed-out queued write is abandoned. Interactive OAuth
  publication is authoritative, supersedes active refreshes, and installs the
  new grant before updating the wizard clone. `Strava::open()` reports success
  only after the complete pair has been durably accepted.
- Verification: All 17 coordinator cases and 34 OAuth/policy/integration cases
  pass normally and under strict ASan/UBSan/LSan. The 11 publication cases,
  71 checked-credential cases, and 105 athlete/cloud handoff cases also pass
  normally and under the same sanitizers. A fresh release build compiles and
  links the complete application and test tree. The full normal matrix passes
  75 suites and 2,517 tests with no failures, skips, or blacklists.

### SEC-016: Transient vault reads can overwrite an unknown newer credential

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp` and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: When a vault read returned `Unavailable` or `Failed` and a legacy
  plaintext credential existed, migration wrote that plaintext value into the
  vault. A transient backend failure cannot prove the vault entry is absent, so
  this could overwrite a newer token or password that the process was
  temporarily unable to read.
- Test-first evidence: With a newer fake-vault credential and older plaintext
  source, both `Unavailable` and `Failed` rows returned the stale plaintext and
  failed the new regression. The focused program otherwise passed 105 cases.
- Resolution: A legacy migration write is now permitted only after an
  authoritative `NotFound`. `Unavailable` and `Failed` preserve both sources,
  return the caller's default, create no cache entry, and retry the vault read
  after recovery. A complementary recovery case verifies that a later
  `NotFound` still migrates a legitimate plaintext-only credential exactly
  once.
- Verification: The focused credential suite passes all 108 cases normally and
  under ASan/UBSan/LSan with leak detection. The complete out-of-source matrix
  runs 2,822 cases across 81 QtTest programs: 2,820 pass, none fail, and the two
  source-contract checks tracked by `TEST-002` skip. The production application
  links as a 536,217,440-byte ELF and remains alive through an eight-second
  offscreen smoke test with an empty temporary home.
- Residual: An entry created concurrently after a successful `NotFound` can
  still race the migration write; that independent atomicity gap is tracked by
  `SEC-018`.

### SEC-017: Credential scopes alias same-named athletes in different roots

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialSettings.h`, `src/Core/Settings.cpp`,
  `src/Core/Settings.h`, `src/Core/main.cpp`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: Legacy credential-scope mapping hashes only the athlete name. Two
  independent athlete roots containing the same directory name can therefore
  adopt one vault scope and expose or overwrite each other's cloud credentials.
  Copied roots and athlete directories could also reuse fresh local bindings
  when their external provenance was not checked.
- Test-first evidence: RED coverage initializes same-named athletes in
  independent roots, copies bound roots and profiles, removes location claims,
  exercises pre- and post-initialization access, and injects failures at every
  identity, claim, binding, and plaintext-migration commit point. Before the
  production fix, same-named roots resolved to one scope, copied fresh bindings
  could bootstrap missing claims, invalid athlete paths disabled a valid active
  root, and authorized legacy global and athlete credentials did not migrate
  safely through interrupted writes.
- Resolution: New credentials are bound to random durable root, profile, and
  scope identities. System-level location claims bind those identities to
  canonical paths and parent identities; existing fresh bindings must match
  pre-existing claims, while only newly generated or explicitly authorized
  legacy identities may create claims. Exact settings paths reject traversal,
  symlinks, dangling links, and Windows reparse points. Legacy plaintext
  migration and binding recovery remain fail closed unless the selected root
  and system claims establish provenance. Command-line and server roots are
  resolved before global settings and credential scopes are initialized.
- Verification: The focused suite passes 241 tests normally, under
  ASan/UBSan/LSan, and under ThreadSanitizer, with zero failures or sanitizer
  reports; five Windows-only ACL/junction cases skip on Linux. MinGW syntax
  checks pass for all changed Core translation units. A complete Qt 6.8 build
  succeeds and all 79 QtTest programs pass with zero failures.
- Residual: Deliberate library moves need a future authenticated rebind
  workflow. Interrupted fresh enrollment is tracked separately by `SEC-021`.
  Same-user filesystem replacement races and unprovable custom legacy-root
  provenance can deny credential access but cannot authorize a copied root.
  Runtime NTFS ACL and junction behavior still requires a Windows worker;
  current Windows verification is cross-compilation plus source-level
  coverage.

### SEC-018: Credential migration uses a non-atomic check-then-write

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialSettings.h`, `src/Core/CredentialStoreQtKeychain.cpp`,
  `src/Core/Settings.cpp`, `src/Core/Settings.h`,
  `unittests/Core/credentialSettings/credentialSettings.pro`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: Migration reads `NotFound` and then performs a normal overwriting
  write. Another thread or process can publish a newer credential between those
  operations, after which the stale plaintext migration overwrites it despite
  the successful initial read. A second legacy source could also win after a
  transient target-vault failure, a copied source could be accepted without
  matching the active root and scope in one snapshot, and an explicitly empty
  target could resurrect an older password on the next read or restart.
- Test-first evidence: Commits `353a512` and `6be0f37` first made a newer value
  appear between the authoritative miss and migration, failed the confirming
  read, and exercised an interrupted creating transaction; the old normal write
  overwrote the newer value or cached the wrong result. Later RED rows proved
  that root, scope, and value could come from different source snapshots, a
  target could appear during fallback, a transient vault failure or cached
  negative result could return an older legacy password over a canonical value,
  and pre-existing or late empty targets could resurrect a superseded
  credential after restart. Fault rows also rejected fallback-marker writes and
  distinguished confirmed vault values from memory-only failed writes.
- Resolution: `CredentialStore` now distinguishes atomic creation outcomes from
  overwriting writes. Migration uses only `createIfAbsent()`, then re-reads and
  returns the canonical value; collision, indeterminate, transient, and
  confirming-read failures never call the overwrite path or scrub the source
  prematurely. The production QtKeychain adapter reports atomic creation as
  unsupported immediately because its current cross-platform job API cannot
  guarantee both create-only semantics and bounded completion. That safe
  fallback keeps the plaintext source usable and retryable instead of
  reintroducing the race. A process-locked file backend proves the contract
  with two simultaneous creators and a deterministic lock-contention
  acknowledgement.
- Source precedence: Cross-file legacy credential content is now a read-only
  fallback. It is eligible only after a live authoritative vault `NotFound`,
  and only when selected root, scope mapping, and credential coexist in one
  readable exact source snapshot. Exact target presence and explicit target
  operations commit a credential-specific durable block marker; empty targets
  remain blocked after their plaintext key is scrubbed and across restart.
  Canonical reads fail closed until that marker is durable, and memory-only
  failed writes are not mistaken for confirmed vault values. Marker and legacy
  files are hardened and verified owner-only before use on Unix. No cross-file
  plaintext is automatically migrated or deleted because the vault and source
  cannot participate in one atomic transaction.
- Verification: The focused program reports 275 passes, zero failures, and five
  expected Windows-only skips normally, under strict ASan/UBSan/LSan with leak
  detection, and under TSan with narrow uninstrumented-Qt event-loop
  suppressions. The production atomic-create probe returns `Unsupported`, while
  the two-process file fixture admits exactly one creator without overwrite.
  The full Qt 6.8.3 application links to a 536,194,112-byte executable, exits
  successfully from `--version`, and remains alive through an eight-second
  offscreen smoke test with an isolated empty home. The complete matrix runs 81
  QtTest suites: 3,018 passed, zero failed, five expected Windows-only skips,
  and zero blacklisted.
- Residual: Automatic legacy-to-vault migration remains disabled on production
  backends until a platform adapter can provide a bounded, truly atomic
  create-only primitive. Cross-file plaintext is retained by design; Unix use
  fails closed if owner-only permissions cannot be verified, while native
  Windows permission behavior still needs a runtime worker. If durable marker
  creation fails, the current read returns the default and retries later; if
  the canonical value disappears before a successful retry, no durable
  precedence decision exists. A target appearing after the final exact
  recheck can cause one stale fallback read but no vault, marker, or source
  mutation. Same-file cleanup and late QtKeychain completion risks are tracked
  separately below.

### SEC-019: Failed plaintext scrubbing is cached as successful

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialSettings.h`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: `scrubPlaintext()` ignores `QSettings::sync()` status. After a failed
  removal sync, the in-memory key is absent while the secret can remain on
  disk; callers then cache the vault result as persisted and may never retry
  scrubbing in that process.
- Test: Reject the settings write used to remove a plaintext credential, reopen
  the file to prove the secret remains, restore writes, and require both
  same-process and restart paths to remove it without altering the vault value.
- Fix direction: Return and propagate checked scrub durability. Do not publish
  a persisted cache state until deletion has synchronized, and retain an
  explicit retry state whenever secure cleanup is incomplete.
- Test-first evidence: Fault-injecting settings backends left plaintext,
  pending-removal markers, and sticky caller status durable after reported
  failures. The RED cases reproduced missed same-process retries, restart
  resurrection, stale positive and negative caches, cross-instance and
  cross-process replacements, grouped settings, path aliases, and concurrent
  credential operations.
- Resolution: Credential metadata now uses fresh exact `QSettings` instances,
  disables fallbacks, requires atomic sync, checks status and postconditions,
  and preserves the caller's active group. Delete intent is committed before
  plaintext or vault removal, and incomplete cleanup remains retryable.
  Nonblocking process-local admission and a per-vault `QLockFile` prevent
  nested-event-loop deadlocks and overlapping processes. Each cache entry is
  bound to a random, atomically committed revision stored in owner-only
  persistent application data; every vault mutation advances that revision
  first, and missing or corrupt revisions get a new baseline before cache
  trust. Scope locks use canonical filesystem identities and normalized
  Windows registry identities.
- Verification: All 150 focused credential cases pass normally, under
  ThreadSanitizer, and under ASan/UBSan/LSan with leak detection. Three
  independent final reviews reported no blocker. The complete out-of-source
  matrix runs 2,864 cases across 81 QtTest programs: 2,862 pass, none fail,
  and the two source-contract checks tracked by `TEST-002` skip. The
  production application links as a 536,296,904-byte ELF and remains alive
  through an eight-second offscreen smoke test with an empty temporary home.

### SEC-020: A second plaintext source can reverse credential deletion

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp` and `src/Core/Settings.cpp`
- Impact: A successful delete scrubs only the plaintext key supplied for that
  operation. If another legacy or fallback settings source still contains the
  same credential for the same vault scope, a later read can migrate that
  duplicate back into the vault and reverse the user's explicit deletion.
- Test: Store one vault credential and duplicate plaintext copies in two exact
  settings sources, delete through one source, reconstruct all settings and
  credential objects, then read through the other source. Require no vault
  write, no returned credential, durable cleanup of every known copy, and a
  later explicit checked replacement to succeed.
- Fix direction: Persist a per-vault deletion tombstone that survives
  successful backend removal and takes precedence over every plaintext
  migration source. Clear it only after a checked replacement has committed,
  and enumerate or journal all known legacy sources so cleanup is resumable.
- Test-first evidence: The committed RED series reproduced cross-source
  resurrection after a completed delete, failed marker and plaintext scrubs,
  stale process caches, revision and directory durability failures, process
  crashes at every mutation boundary, incomplete initial and replacement
  writes, and delete preparations that lost their creation/update ancestry.
  The final lineage regressions in `c08d257` failed in both the same-source and
  cross-source creation cases while the active-credential anti-resurrection
  control remained green. Windows contracts in `4931849`, `4e87cd2`, and
  `3be8314` were committed before the corresponding ACL and handle changes.
- Resolution: Every vault key now has secret-free, atomically replaced
  transaction state and a generation-bound settings marker. Delete intent,
  revision changes, plaintext cleanup, vault mutation, and final state are
  ordered so a restart can resume without resurrecting a deleted credential or
  discarding a recoverable legacy credential. Distinct creation, update, and
  post-deletion replacement phases preserve ancestry across failed operations;
  legacy `preparing` state remains readable and resolves conservatively as an
  update. Cache entries are tied to durable revisions and transactions, and a
  canonical per-vault process lock serializes competing processes. Unix writes
  flush files and every newly created ancestor; Windows uses write-through
  replacement, persistent-ACL volumes, protected owner-only inheritable DACLs,
  no-follow file access, and retained root/application/lock directory handles
  that deny rename or replacement during an operation. Existing permissive
  private directories fail closed instead of making their prior contents
  trusted by retroactive hardening.
- Verification: The focused credential program passes 205 cases normally,
  under strict ASan/UBSan/LSan, and under ThreadSanitizer, with no failures and
  four Windows-only skips on Linux. Both the production file and Windows test
  branches pass a MinGW C++17 syntax build. The complete out-of-source matrix
  runs 81 QtTest programs: 2,917 cases pass, none fail, and six
  platform/source-contract cases skip. The complete Linux application links as
  a 536,376,608-byte ELF.
- Residual: The Windows-only ACL tests still require a native, non-administrator
  NTFS CI run; MinGW validates compilation but cannot execute those WinAPI
  contracts on this Linux build host.

### SEC-021: Interrupted fresh credential enrollment is not recoverable

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`, `src/Core/CredentialSettings.h`,
  `src/Core/Settings.cpp`, `src/Core/Settings.h`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: Fresh root identities and scope bindings are persisted before their
  system location claims. A crash or transient claim-write failure in between
  leaves valid local metadata that is deliberately forbidden from recreating a
  missing claim. Every later attempt therefore fails closed until metadata is
  repaired manually. Credentials are not exposed, but new credential
  enrollment and access can be denied permanently.
- Test-first evidence: Fifteen process-exit injection points initially exposed
  non-recoverable root, global-scope, athlete-profile, and athlete-scope
  publication windows. The first metadata-loss matrix passed only 8 of 14
  cases, and its partial recovery implementation then exposed unsafe
  reenrollment when both local identifiers were absent. Independent RED tests
  also reproduced two competing parents for one pending location, acceptance
  of a canonical claim stored under the wrong key, incomplete tuple validation
  during completion, and fresh enrollment beside a canonical same-location
  claim owned by another parent. The last case returned `Success` before the
  parent-conflict fix. A deterministic two-process test now makes exactly one
  enrollment API call per child and proves actual lock contention separately
  for root, global scope, athlete profile, and athlete scope.
- Resolution: Fresh enrollment now creates a durable, canonical-location-bound
  intent and claim in an external authority before publishing local metadata.
  Completion atomically converts the intent to a permanent location binding
  only after an exact local root/profile/scope tuple is visible. Whole-map
  settings transactions bypass stale `QSettings` caches, use a cross-process
  lock with bounded waiting, atomically replace the authority file, harden its
  ownership and link semantics, flush the file and required ancestors, and
  refresh the caller only after durable publication. Restart recovery reuses
  the pending identity, reconstructs either missing half of otherwise
  authenticated local metadata, and refuses reenrollment when both local
  identity components are gone. Canonical claims, intents, and permanent
  bindings authenticate their hashed storage keys and reject malformed
  records, copied locations, competing parents, symlink/reparse aliases, and
  hard-linked authority files without mutating plaintext or the vault.
- Verification: The complete credential program passes 382 cases normally and
  the same 382 cases under strict ASan/UBSan/LSan with leak detection; neither
  run has failures and both have seven platform-only skips. A 58-case SEC-021
  ThreadSanitizer matrix passes without a race report. Both the production file
  and all Windows test branches pass a MinGW C++17 syntax build. The complete
  out-of-source matrix runs 81 QtTest programs: 3,126 cases pass, none fail or
  are blacklisted, and seven platform-contract cases skip. The complete Linux
  application links as a 536,721,920-byte ELF.
- Residual: Location ownership intentionally uses a canonical pathname rather
  than a persistent filesystem object identifier, so replacing a directory at
  the identical pathname is outside this same-user recovery boundary. The
  Windows authority-junction, hard-link, case-alias, ACL, and write-through
  contracts compile under MinGW but still require execution on native,
  non-administrator NTFS CI. A process that holds the authority lock longer
  than the bounded wait causes a transient fail-closed result and a later
  retry, not an unbounded UI stall.

### SEC-022: Same-file credential cleanup can delete a newer or last copy

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialSettings.h`, `src/FileIO/AtomicFileWriter.h`,
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`, and
  `unittests/FileIO/atomicActivitySave/testAtomicActivitySave.cpp`
- Impact: Credential migration and canonical reads take a plaintext or vault
  snapshot and later scrub the plaintext key without atomically binding those
  operations. Another writer can replace the plaintext after the snapshot and
  have its newer value deleted, or an external actor can remove the vault value
  after its successful read and leave cleanup removing the last remaining
  credential copy.
- Test-first evidence: The initial fresh-snapshot regressions failed in three
  independent ways: a concurrent replacement was deleted, a plaintext key
  hidden by QSettings' stale negative cache survived cleanup, and a crash left a
  named plaintext snapshot behind. Reverting the direct scrub preflight also
  made `plaintextRemovalBypassesStaleNegativeCache` delete the vault while
  retaining plaintext. Earlier authorization coverage proved that an unrelated
  nonempty vault value could authorize deletion and that persisted intent could
  delete a replacement after restart. The shared atomic-writer regression
  rejects a publisher that reports success without publishing its target.
  Windows-only crash coverage requires every retained serialized plaintext copy
  to have a protected, non-inherited, owner-only DACL; that contract exposed and
  fixed a crash point placed before final DACL hardening.
- Resolution: Same-file cleanup now runs under the settings lock and atomically
  replaces a complete exact settings map. Secret-free `intent`, `authorized`,
  `conflict`, and `complete` sidecar phases bind recovery to the canonical
  settings/key identity and a source generation. Persisted authorization alone
  never permits deletion of mismatched plaintext: that exceptional path also
  requires the initiating call's stack-local target secret, an unchanged source
  generation, and a final matching vault read. Normal cleanup reads the source
  twice through secure fresh file handles, bypassing QSettings caches, and
  refuses symlink/reparse, hard-link, metadata, or content changes. Unix parsing
  uses an owner-only private scratch directory and an immediately unlinked
  descriptor exposed through a unique `/proc/self/fd` or `/dev/fd` path.
  Windows staging is hardened before plaintext is written or made
  crash-observable, and open native temporary-file handles are destroyed before
  `MoveFileExW`. Scratch cleanup is globally locked and recovers directories
  only after acquiring their active lock or proving the same-host owner process
  dead. Atomic publication now also rejects a successful callback that did not
  report the target as published.
- Verification: The final focused credential program passes 326 cases normally,
  under ASan/UBSan/LSan, and under ThreadSanitizer, with no failures, sanitizer
  reports, or races and seven platform-only skips on Linux. Production and test
  Windows branches pass a MinGW64 C++17 syntax check. The atomic activity-save
  program passes 72 cases with no skips. The complete out-of-source matrix runs
  81 QtTest programs: 3,070 cases pass, none fail, and seven platform cases
  skip. The complete Linux application links as a 536,628,600-byte ELF.
- Residual: The final successful vault read is the cleanup linearization point;
  an arbitrary out-of-band vault deletion after it cannot be coordinated
  without backend compare-and-swap or lease support. Writers that bypass the
  settings lock, parent-directory replacement, hard-link creation, and metadata
  ABA remain outside the cooperating-writer contract. Native non-administrator
  NTFS execution is still required for the Windows DACL, sharing, and
  `MoveFileExW` runtime branches; MinGW validates syntax only. Native macOS CI is
  still needed for the `/dev/fd` parsing path. A process crash may retain a
  private scratch directory until the next credential snapshot operation;
  dead-process recovery is covered on Linux.

### THREAD-013: Timed-out QtKeychain jobs can mutate the vault later

- Status: OPEN
- Code: `src/Core/CredentialStoreQtKeychain.cpp`
- Impact: A timed-out QtKeychain job is switched to auto-delete and released
  while its backend operation continues. The caller receives `Unavailable` and
  releases the per-credential operation lock, but a timed-out write or removal
  can still complete later and reorder against a retry, replacement, or delete.
- Test: Delay fake read, write, and remove jobs beyond the timeout, start a
  conflicting operation after the reported failure, and then release the first
  job. Require no late vault mutation or callback after the operation guard has
  ended.
- Fix direction: Provide backend cancellation with a terminal acknowledgement,
  or retain serialized ownership until the job reaches a terminal state. An
  indeterminate mutation needs durable recovery state rather than a definite
  `Unavailable` result.

## Medium

### SEC-023: External vault mutations bypass credential cache revisions

- Status: OPEN
- Code: `src/Core/CredentialSettings.cpp`
- Impact: Cache entries are invalidated by GoldenCheetah's revision sidecar, but
  direct keychain changes by another application do not advance that revision.
  Normal reads can therefore return a stale positive or negative value until a
  local mutation, cache clear, or restart. SEC-018 now bypasses cached misses
  when authorizing cross-file fallback, but ordinary credential reads retain
  the broader stale-cache behavior.
- Test: Prime positive and negative cache entries, mutate the fake vault without
  touching the revision sidecar, and require the next policy-relevant read to
  observe the external value or deletion.
- Fix direction: Add backend change notifications or a backend generation to
  the cache key. Where neither is available, use bounded cache lifetimes or
  live reads for decisions that can expose, overwrite, or delete credentials.

### MEM-019: Indented plot marker starts and copies its matrix from indeterminate state

- Status: FIXED
- Code: `src/Charts/IndendPlotMarker.cpp:26`,
  `src/Charts/IndendPlotMarker.cpp:39`,
  `src/Charts/IndendPlotMarker.cpp:50`,
  `src/Charts/IndendPlotMarker.cpp:89`,
  `src/Charts/IndendPlotMarker.cpp:97`,
  `src/Charts/IndendPlotMarker.cpp:120`, and
  `src/Charts/IndendPlotMarker.h:88`
- Impact: The shared marker matrix constructor leaves `m_canvasId`
  uninitialized and the first label draw reads it. Its public copy constructor
  also compares uninitialized dimensions and can pass an uninitialized
  `m_data` pointer to `delete[]` through `resize()`. No copy call exists in the
  current tree, but the first-draw read is active and the callable copy path is
  immediate undefined behavior.
- Test-first evidence: A focused test constructed the matrix in `0xa5`-filled
  storage and observed a nonzero initial canvas identity. Copy construction in
  equally poisoned storage then crashed with `SIGSEGV`. A rectangular matrix
  exposed the row-stride indexing error, and copy assignment first lost the
  expected pixel before aborting on a double free.
- Resolution: Constructors now initialize every member, and the complete Rule
  of Three performs independent, canvas-preserving deep copies without reading
  destination storage. Assignment and resize allocate before replacing owned
  storage, self-assignment is harmless, reset restores the canvas identity, and
  checked writes use the column stride.
- Verification: All 7 focused cases pass normally and under
  ASan/UBSan/LSan with no sanitizer report. A clean GCC 13 release build has 0
  errors and emits no `IndendPlotMarker` warning, and the complete release
  matrix passes 72 suites and 2,448 tests (0 failed, skipped, or blacklisted).

### BLE-006: Kinetic InRide UUID fallback violates aliasing and alignment

- Status: FIXED
- Code: `src/Train/KurtInRide.cpp`,
  `unittests/Train/kineticPacketBounds/kineticPacketBounds.pro`,
  `unittests/Train/kineticPacketBounds/testKineticPacketBounds.cpp`
- Impact: When Qt cannot provide a Bluetooth address, notably on macOS, the
  Kinetic InRide system ID fallback reads a `quint128` object through a
  `uint64_t*`. The incompatible and potentially under-aligned typed access is
  undefined behavior under optimization and can derive the wrong six-byte ID,
  breaking telemetry decoding and calibration commands.
- Test-first evidence: Device-info fixtures cover a known Bluetooth address and
  a null address plus a UUID whose first six bytes have their high bits set.
  With optimized GCC 13.3 and `-Werror=strict-aliasing`, the RED build stopped
  at the type-punned `uint64_t` fallback read before the production fix.
- Resolution: The address path now derives its six least-significant bytes with
  explicit shifts, independent of host byte order. The UUID fallback copies the
  first six RFC 4122 bytes without typed aliasing or alignment assumptions.
  A GCC-only optimized warning gate prevents the unsafe cast from returning.
- Verification: All 114 focused QtTest results pass normally and under
  ASan/UBSan/LSan, including exact address and UUID byte assertions. The full
  application links, and the complete matrix passes 79 test programs and 2,699
  tests with zero failures, skips, or blacklisted results. The replacement API
  is available in the project's minimum supported Qt 6.5.3.

### BLE-007: Silent heart-rate streams are not recovered independently

- Status: FIXED
- Code: `src/Train/BT40Device.cpp`, `src/Train/BT40Device.h`,
  `src/Train/BT40Controller.cpp`, `src/Train/BluetoothTelemetryRouter.cpp`, and
  `unittests/Train/bt40Lifecycle`
- Impact: A Bluetooth heart-rate link can remain logically connected after
  notifications stop. The Train view then records repeated zero-heart-rate
  periods until a manual disconnect/connect cycle, even though trainer
  telemetry continues.
- Evidence: One captured workout contains seven heart-rate-only gaps lasting
  33-53 seconds. A separate 15-second all-telemetry gap aligns with an operating
  system Bluetooth-adapter reset, but no adapter or trainer interruption
  explains the seven isolated heart-rate gaps. The sensor resumes after a
  manual application reconnect and does not show the same behavior in other
  applications. A later independent recording reproduced multiple 15-53 second
  heart-rate gaps while trainer telemetry remained available.
- Root cause: Raw BLE `connected` previously published `connectionRestored`,
  stops reconnect timing, and clears rediscovery before HR service discovery or
  CCC subscription succeeds. Five-second telemetry expiry only clears the
  displayed value; invalid services, missing HR characteristics/descriptors,
  and descriptor-write errors do not enter recovery. Reconnect callbacks also
  do nothing while Qt remains in any Connecting, Connected, Discovering, or
  Discovered state.
- Test: Require HR silence while link-connected to start bounded recovery; keep
  `connectionRestored` absent until HR subscription or a valid sample; cover
  invalid service, missing characteristic/descriptor, descriptor-write error,
  and a connection error stuck in Connecting; and retain rediscovery until a
  usable HR stream exists.
- Test-first evidence: The RED lifecycle builds accepted a raw connection as
  restored, retained stale samples, skipped queued disconnects, repeatedly
  reused failed or stuck controllers, cancelled rediscovery too early, and
  crashed on a null service object. A slow but progressing connection was also
  torn down on the first retry tick.
- Resolution: Heart-rate stream readiness is now independent of the Qt link
  state. A 15-second startup watchdog and 10-second sample watchdog require an
  actual valid heart-rate notification before publishing restoration. Silence,
  incomplete GATT discovery, an invalid service, a missing measurement or CCC
  descriptor, subscription failure, and connection errors clear only that
  telemetry source and enter bounded recovery. Active links are closed before
  reconnect, rediscovery remains pending until samples resume, and a manual
  disconnect cancels pending scans. Three failed connection attempts or stalled
  retry ticks replace the low-energy controller while preserving the configured
  address type.
- Verification: All 50 lifecycle cases pass normally, under strict
  ASan/UBSan/LSan, and under TSan with uninstrumented Qt modules excluded. The
  full Qt 6.8.3 application links, all 79 test programs pass, and the production
  binary remains running in an isolated offscreen smoke test with a clean home
  directory.

### BLE-008: Bluetooth adapter resets leave BLE devices without recovery

- Status: OPEN
- Code: `src/Train/BT40Device.cpp`, `src/Train/BT40Controller.cpp`, and
  `unittests/Train/bt40Lifecycle`
- Impact: Scans return while the local adapter is invalid, with no
  adapter-availability backoff or controller-wide recovery coordinator. After
  a USB adapter reset, trainer and sensor telemetry can remain unavailable until
  the application or connection is restarted manually.
- Evidence: One captured workout has a 15-second gap across trainer and
  heart-rate telemetry at the same time as an operating-system Bluetooth USB
  adapter reset. This is separate from the seven heart-rate-only gaps tracked
  by `BLE-007`.
- Partial resolution: `InvalidBluetoothAdapterError` now enters bounded
  heart-rate recovery, preserves the remote address type, and eventually
  replaces that device's controller. Adapter restoration and trainer recovery
  are still not coordinated end to end.
- Test: Invalidate the adapter during an active connection, restore it, and
  require bounded rescanning, controller recreation, and independent recovery
  of all configured BLE devices without a user disconnect/connect cycle.
- Fix direction: Keep adapter-invalid devices pending, retry adapter discovery
  with backoff, and recreate per-device controllers only after a valid adapter
  is available.

### BLE-009: Low-energy controller handoff overlaps BlueZ connections

- Status: FIXED
- Code: `src/Train/BT40Device.cpp`, `src/Train/BT40Device.h`, and
  `unittests/Train/bt40Lifecycle`
- Impact: Replacing a failed controller while its connection still exists can
  overlap two BlueZ connection attempts for one physical device. Deleting the
  old controller directly from its synchronous error callback can also destroy
  the active signal sender, while a controller that never emits `disconnected`
  can otherwise remain allocated indefinitely.
- Test-first evidence: The RED fake-controller tests observed overlapping live
  controllers, synchronous sender destruction, and unbounded retirement. They
  also exercised manual disconnect and device destruction during the handoff.
- Resolution: Replacement is now a two-phase handoff. The stale controller is
  detached from the device, disconnected from callbacks, and retired with
  `deleteLater`; a fresh controller is created only from the stale controller's
  queued `destroyed` completion. Shutdown retirement has a 30-second fallback,
  and manual disconnect or device destruction cancels a pending replacement.
- Verification: The focused 50-case normal, ASan/UBSan/LSan, and TSan runs
  report no overlap, synchronous destruction, leak, or race. The full build and
  79-program matrix pass.

### BLE-010: Stale GATT callbacks can mutate a replacement connection

- Status: FIXED
- Code: `src/Train/BT40Device.cpp`, `src/Train/BT40Device.h`, and
  `unittests/Train/bt40Lifecycle`
- Impact: Queued controller or service callbacks can arrive after disconnect or
  replacement and publish stale heart-rate data, cancel recovery, or mutate
  current trainer state. A null result from `createServiceObject` was appended
  and later dereferenced during service scanning.
- Test-first evidence: A queued old-service notification restored telemetry
  after disconnect, queued controller signals changed recovery state, and the
  null-service fixture crashed with `SIGSEGV`.
- Resolution: Controller callbacks now require the current sender and a
  compatible controller state. Service callbacks require membership in the
  current GATT service set, and services are disconnected and cleared at every
  link transition. Null service objects are logged and ignored.
- Verification: Real 8-bit and 16-bit heart-rate notification payloads still
  route through the production callback, while stale and malformed callbacks
  are ignored. All focused sanitizer runs, the full build, and the complete
  test matrix pass.

### BLE-011: Trainer control-service pruning leaks and depends on discovery order

- Status: FIXED
- Code: `src/Train/BT40Device.cpp` and
  `unittests/Train/bt40Lifecycle`
- Impact: When a lower-priority controllable service is discovered after the
  selected higher-priority service, it remains active. Services removed in the
  opposite order were detached from the list without being deleted. A
  multi-service trainer can therefore retain conflicting control paths and leak
  service objects on each discovery.
- Test-first evidence: Data-driven RED cases exercised both discovery orders
  and required every discarded service object to be destroyed.
- Resolution: The scan now keeps exactly one highest-priority controllable
  service regardless of discovery order, removes it from initialization state,
  disconnects its callbacks, and deletes every discarded object.
- Verification: Both ordering cases pass with exact destruction counts in the
  focused normal and sanitizer suites; the full application and test matrix
  also pass.

### DATA-002: Merge offsets treat samples as seconds

- Status: FIXED
- Code: `src/Gui/MergeActivityWizard.cpp`,
  `src/Gui/MergeActivityTimeOffset.h`,
  `src/Gui/MergeActivityTimeOffset.cpp`, `src/src.pro`,
  `unittests/Gui/mergeActivityTimeOffset/mergeActivityTimeOffset.pro`,
  `unittests/Gui/mergeActivityTimeOffset/testMergeActivityTimeOffset.cpp`,
  `unittests/unittests.pro`
- Impact: Offsets are stored in samples, but interval timestamps and the UI add
  or display them as seconds. For activities whose recording interval is not
  one second, merged intervals are shifted by the wrong duration.
- Test-first evidence: The new focused target initially failed to build because
  `MergeActivityTimeOffset.cpp` did not exist (`make` exit 2). A subsequent
  zero-adjustment regression failed because the helper returned negative zero
  and `std::signbit(adjustment)` was true before zero normalization was added.
- Regression coverage: With `recIntSecs=2`, an offset of three samples shifts
  interval `[10, 20]` to `[16, 26]` and renders the slider adjustment as
  `-6 secs`; the opposite slider direction renders `6 secs`. Additional cases
  cover positive zero and a fractional 0.5-second recording interval.
- Resolution: Merge offsets remain sample-indexed for data-point selection. A
  shared pure helper now converts them through `recIntSecs` when shifting
  interval timestamps or rendering the time adjustment, and normalizes the
  zero display so the UI cannot render `-0 secs`.
- Verification: All six focused QtTest results passed normally and under strict
  ASan/UBSan/LSan, with no sanitizer reports. A fresh full release build and all
  62 unit-test projects passed (2,021 passed, 0 failed, 0 skipped,
  0 blacklisted). The 166,496,760-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `57067168fd1ceae0a25eacaf3e50d85727c832b08b431225a0f30acbd6908dff`.
  It remained stable for a 15-second isolated X11 launch and a 45-second launch
  against a copied real athlete profile, with no crash or new runtime error.

### DATA-003: Merge leaves XData timestamps unshifted

- Status: FIXED
- Code: `src/Gui/MergeActivityWizard.cpp`,
  `src/Gui/MergeActivityXData.h`, `src/Gui/MergeActivityXData.cpp`,
  `src/src.pro`,
  `unittests/Gui/mergeActivityXData/mergeActivityXData.pro`,
  `unittests/Gui/mergeActivityXData/testMergeActivityXData.cpp`,
  `unittests/unittests.pro`
- Impact: Normal samples are shifted during merge, but XData points are copied
  with their original timestamps. Auxiliary data therefore becomes
  permanently misaligned whenever either ride has a nonzero offset.
- Test-first evidence: The behavior-preserving deep-copy implementation failed
  three focused regressions (`tst_mergeActivityXData` exit 3): a source point at
  four seconds remained at four instead of moving to ten, all four boundary
  probes survived instead of two, and an invalid empty timeline retained a
  point. The target then passed only after timestamp shifting and clipping were
  implemented.
- Regression coverage: With `recIntSecs=2` and a three-sample offset, a unique
  XData point moves from four to ten seconds while metadata, distance, numeric
  and string payloads survive a deep copy and the source stays unchanged.
  Shifted points exactly at the zero and terminal boundaries remain; points
  outside them, non-finite timestamps, and null entries are excluded. An empty
  merged timeline preserves series metadata without retaining points.
- Resolution: Both source rides now deep-copy XData through a shared helper that
  reuses the sample-to-seconds conversion from DATA-002, applies the owning
  ride's sample offset, and clips against the inclusive combined sample
  timeline. Existing first-series-wins behavior for duplicate XData names is
  unchanged.
- Verification: All six focused QtTest results passed normally in 0 ms and under
  strict ASan/UBSan/LSan in 1 ms, with no sanitizer reports. A fresh full
  release build and all 63 unit-test projects passed (2,027 passed, 0 failed,
  0 skipped, 0 blacklisted). The 166,492,664-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `7c7695764ac9a0747af03e31764c8490cf871b03ba4ca7b736fb4be180541b2c`.
  It remained stable for a 15-second isolated X11 launch and a 45-second launch
  against a copied real athlete profile; logs contained only translator debug
  notices.

### DATA-004: Constant shared series selects an artificial alignment

- Status: FIXED
- Code: `src/Gui/MergeActivityAlignment.cpp`,
  `unittests/Gui/mergeActivityAlignment/testMergeActivityAlignment.cpp`
- Impact: Excluded samples still inflate the legacy mean denominator, so two
  identical nonzero constant series receive an artificial `R^2=1` and usually
  select `-floor(samples/3)`. This can override a genuinely alignable varying
  series and shift the merged activity by minutes.
- Test-first evidence: The expanded focused suite failed five regressions
  against the old implementation (`tst_mergeActivityAlignment` exit 5): the
  corrected direct and FFT reference scores disagreed with production, a
  constant series and a constant overlap both produced candidates, and a
  constant batch member with key 9 displaced the correctly shifted varying
  member with key 7.
- Regression coverage: Constant nonzero series are rejected on both the direct
  and FFT paths, as are all-zero series and overlaps that are constant even
  though both complete inputs vary. A batch ignores a constant member in favor
  of a varying series shifted by 73 samples. Deterministic fixtures cover
  positive and negative offsets, the direct/FFT boundary, more than 64 tied
  exact candidates, series tie ordering, cooperative cancellation, runner
  lifetime, event-loop responsiveness, and one- and three-hour inputs.
- Resolution: Mean accumulation now counts only samples that actually
  participate in the legacy base-tail mean. Both exact and FFT scoring reject
  non-finite values, empty or zero-variance overlaps, and non-positive total
  variance. The FFT path tracks changes with prefix counters so candidate
  variance checks remain constant-time, while its exact recheck and strict
  comparison preserve deterministic offset and series tie ordering.
- Verification: All 18 focused QtTest results passed normally in 129 ms and
  under strict ASan/UBSan/LSan in 244 ms, with no sanitizer reports. A fresh
  full release build and all 63 unit-test projects passed (2,029 passed,
  0 failed, 0 skipped, 0 blacklisted). The 166,492,664-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `9a458994c5ed9f79c5968ee62a1da7892a72f0d4118feed6cdcb64e5ce4fe611`.
  It remained stable for a 15-second clean-profile launch and a 45-second
  launch against a copied real athlete profile; the logs contained only
  missing translator debug notices.

### DATA-005: zones() validation and evaluation disagree on literals

- Status: FIXED
- Code: `src/Core/DataFilter.cpp`, `src/Core/DataFilterSafety.cpp`,
  `src/Core/DataFilterSafety.h`, `src/Core/DataFilterZones.cpp`,
  `src/Core/DataFilterZones.h`, `src/src.pro`,
  `unittests/Core/dataFilterSafety/testDataFilterSafety.cpp`,
  `unittests/Core/dataFilterZones/DataFilterParserTestStubs.cpp`,
  `unittests/Core/dataFilterZones/dataFilterZones.pro`,
  `unittests/Core/dataFilterZones/testDataFilterZones.cpp`, and
  `unittests/unittests.pro`
- Impact: `zones()` accepts series and field names case-insensitively but
  evaluates them with case-sensitive comparisons, so expressions such as
  `zones(POWER,NAME)` are accepted and silently return empty data. Invalid
  literals inside a nested expression can also escape validation because the
  validator sets its own `inerror` member instead of `leaf->inerror`.
- Evidence: Exhaustive branch tracing proves the adjacent GCC
  `maybe-uninitialized` diagnostics are false positives: every pointer and
  `metricpace` read is dominated by its matching `series` assignment, while an
  unknown series keeps every dereferencing loop disabled. The case and error
  target mismatches are independent reachable logic defects.
- Test-first evidence: The first focused build failed because the new
  `ZoneArguments` contract did not exist. A second test target using the real
  Flex/Bison grammar then failed because the AST-level `DataFilterZones`
  adapter was absent. These RED builds established both the complete literal
  matrix and parser-produced root and nested AST cases before the production
  implementation. Independent review then found that parent functions such as
  `cumsum()` did not traverse their parameters during semantic validation.
  A malformed-AST regression crashed with `SIGSEGV` at the unchecked null
  parameter, and the skipped-parent regression failed to compile until the
  complete-tree validation API existed.
- Resolution: A single normalizer now validates all four series and seven
  fields and returns canonical lowercase values. Validation writes those
  values back to the parsed AST and marks the exact leaf being checked;
  evaluation independently normalizes defensively before dispatch. Malformed
  calls, including null AST members, return an empty numeric result instead of
  indexing invalid parameters. After the existing parent-specific semantic
  validation, a complete ownership-aware AST pass canonicalizes every
  remaining nested call and reports only newly discovered invalid leaves. All
  three formula parse/check entry points use that pass. Branch-local zone
  pointers and the pace-unit flag are explicitly initialized without changing
  the existing zone lookup behavior.
- Verification: All 45 literal-helper results and all 31 parser/AST results
  pass normally and under ASan/UBSan/LSan with no sanitizer report. The AST
  suite directly covers every child-ownership route handled by `Leaf::clear`.
  The full release application links to a 536,090,168-byte binary, and the
  complete matrix passes 80 test programs and 2,764 tests with zero failures,
  skips, or blacklisted results.

### DATA-006: Voronoi annotations accept duplicate and non-finite sites

- Status: FIXED
- Code: `src/Charts/GenericPlot.cpp`, `contrib/voronoi/Voronoi.cpp`,
  `contrib/voronoi/Voronoi.h`,
  `unittests/Charts/voronoiSafety/testVoronoiSafety.cpp`,
  `unittests/Charts/voronoiSafety/voronoiSafety.pro`, and
  `unittests/unittests.pro`
- Impact: Formula-provided Voronoi centers are checked only for an even numeric
  count. Duplicate sites can make the bisector divide by zero, while NaN or
  infinite coordinates invalidate sorting and geometry invariants. Non-finite
  lines can then reach the chart renderer.
- Test-first evidence: The first focused build failed because `addSite()` did
  not report rejected input. With only that interface added, 4 tests passed and
  7 failed: NaN, both infinities, a value beyond `float`, exact duplicates, and
  coordinates that collapse to the same stored `float` were all accepted.
  Subsequent RED cases exposed a non-finite intersection, the missing
  `run()` result contract, and the zero-width hash calculation used by vertical
  collinear sites.
- Resolution: Sites are range-checked before conversion, stored-coordinate
  duplicates are rejected, and the chart requires equal coordinate-vector
  lengths plus at least two accepted sites. Invalid annotations now skip only
  themselves rather than all later annotations. The sweep reports failure,
  clears partial output, guards zero-width buckets and zero-length bisectors,
  aborts when a geometry invariant fails, and never emits a non-finite line.
- Verification: All 27 focused results pass normally and under strict
  ASan/UBSan/LSan with leak detection. A fresh release application links to a
  536,103,744-byte binary. The complete offscreen matrix passes 81 test
  projects and 2,791 tests with zero failures, skips, or blacklisted results.

### DATA-007: Function parsing leaks consumed symbol nodes

- Status: FIXED
- Code: `src/Core/DataFilter.y`,
  `unittests/Core/dataFilterZones/DataFilterParserTestStubs.cpp`,
  `unittests/Core/dataFilterZones/dataFilterZones.pro`, and
  `unittests/Core/dataFilterZones/testDataFilterZones.cpp`
- Impact: Every generic function call with parameters leaked the temporary
  symbol node holding its function name. Function definitions leaked the same
  node through an adjacent grammar action, so repeated formula parsing grew
  process memory even after each syntax tree was destroyed.
- Test-first evidence: The parser integration tests first passed all 10
  functional assertions but exited with LeakSanitizer failure. LSan reported
  1,712 leaked bytes in 30 allocations rooted at the `SYMBOL` grammar action;
  nested calls leaked one node and one string per consumed function name.
- Resolution: Both grammar actions now copy the function name and immediately
  release the consumed symbol string and node. The no-argument function action
  continues to reuse its symbol node and therefore retains its existing
  ownership path.
- Verification: Parser-produced parameterized calls, nested calls, a function
  definition, and a syntax error after successful function reduction now pass
  in the 31-case parser/AST suite normally and under ASan/UBSan/LSan without
  leaks, use-after-free, or double-free. The full release application links,
  and the complete matrix passes 80 test programs and 2,764 tests with zero
  failures, skips, or blacklisted results.

### DATA-008: Voronoi allocator accounting starts indeterminate

- Status: FIXED
- Code: `contrib/voronoi/Voronoi.cpp`,
  `unittests/Charts/voronoiSafety/testVoronoiSafety.cpp`
- Impact: The constructor initialized only three mode flags. Allocation
  accounting, search counters, bounds, and internal pointers retained whatever
  bytes occupied the object. The first allocation added its size to an
  indeterminate signed integer, which is undefined behavior and could also
  produce misleading diagnostics.
- Test-first evidence: A placement-new regression poisoned object storage with
  `0xa5` before construction. The old constructor left `siteidx`, `ntry`,
  `totalsearch`, and `total_alloc` at `-1515870811` instead of zero.
- Resolution: The initializer list now establishes every scalar, pointer, and
  freelist member before the first allocator operation. The sweep's cached
  priority point is also value-initialized defensively.
- Verification: Constructor accounting is covered directly in the 27-result
  normal and sanitizer suites. The full release build and 2,791-test matrix
  pass.

### DATA-009: Large finite Voronoi coordinates overflow float intermediates

- Status: FIXED
- Code: `contrib/voronoi/Voronoi.cpp`,
  `contrib/voronoi/Voronoi.h`,
  `unittests/Charts/voronoiSafety/testVoronoiSafety.cpp`
- Impact: Coordinates such as `1e20` are finite and representable as `float`,
  but distance squares, intersections, side predicates, clipping, and plot
  margins performed intermediate arithmetic in `float`. They could overflow
  or change sweep ordering even when every input and final result was valid.
- Test-first evidence: Three new large-coordinate regressions failed together:
  a representable intersection was discarded, a finite distance became
  infinite, and `right_of()` returned the opposite result. Independent review
  then found a subtler binary32 counterexample near `FLT_MAX`; the focused
  suite passed 26 cases but failed that exact predicate because arithmetic was
  rounded before assignment to a `double`.
- Resolution: Bisectors, intersections, distances, side predicates, queue
  priorities, plot bounds, and clipping now use explicitly promoted `double`
  intermediates. Every conversion back to legacy `float` storage is checked,
  queue insertion is failure-atomic, and unrepresentable geometry aborts the
  diagram with no partial lines.
- Verification: Exact binary32 boundary fixtures, large finite diagrams,
  invalid priorities, unrepresentable intersections and bisectors, randomized
  sites, and finite-output invariants all pass in the 27-result focused suite
  normally and under ASan/UBSan/LSan. Independent final review found no
  remaining blocker. The full release build and 2,791-test matrix pass.

### MEM-022: Voronoi copying and sweep reuse alias arena storage

- Status: FIXED
- Code: `contrib/voronoi/Voronoi.cpp`,
  `contrib/voronoi/Voronoi.h`,
  `unittests/Charts/voronoiSafety/testVoronoiSafety.cpp`
- Impact: The implicit copy operations duplicated raw arena pointers and the
  allocation list, so destroying both objects could free the same blocks
  twice. A second sweep or post-sweep `addSite()` could also inspect site
  records already returned to the freelist while `sites` retained their
  addresses.
- Test-first evidence: The ownership trait regression failed because `Voronoi`
  was copy-constructible and copy-assignable. After copy ownership was closed,
  a lifecycle regression still failed because `addSite()` accepted a new point
  after a completed sweep.
- Resolution: Copy construction and assignment are deleted. A sweep may start
  only once; all later runs and site additions are rejected. A failed
  precondition with fewer than two sites remains recoverable, while any
  actually started sweep, successful or failed, consumes the instance.
- Verification: Compile-time ownership checks and successful, insufficient,
  and failed sweep transitions pass normally and under ASan/UBSan/LSan. The
  full release build and 2,791-test matrix pass.

### PARSE-001: ZIP/GZIP decompression has no resource limits

- Status: FIXED
- Code: `contrib/qzip/zip.cpp`, `contrib/qzip/zipreader.h`,
  `src/FileIO/CompressedActivityFile.cpp`,
  `src/FileIO/CompressedActivityFile.h`, `src/FileIO/RideFile.cpp`,
  `src/Cloud/CloudService.cpp`, `src/FileIO/AthleteBackupArchive.cpp`,
  `src/src.pro`,
  `unittests/FileIO/archiveSecurity/archiveSecurity.pro`,
  `unittests/FileIO/archiveSecurity/testArchiveSecurity.cpp`,
  `unittests/Core/athleteMigrationSafety/athleteMigrationSafety.pro`,
  `unittests/Core/athleteMigrationSafety/testAthleteMigrationSafety.cpp`,
  `unittests/FileIO/athleteBackupArchive/athleteBackupArchive.pro`,
  `unittests/FileIO/athleteBackupArchive/testAthleteBackupArchive.cpp`
- Impact: Compression bombs can exhaust memory or freeze the UI.
- Test-first evidence: Against the old implementation, the archive suite passed
  26 tests and failed four resource-limit regressions covering entry count,
  per-entry output, aggregate output, and compression ratio. Two Cloud import
  regressions also failed because hostile ZIP and GZIP payloads reached the
  activity parser. A subsequent review added compressed-input and central
  metadata budgets first; those three new ZIP/GZIP tests failed before their
  enforcement was implemented. A final compatibility review reproduced a
  DOS-created ZIP member without Unix file-type bits; that regression passed
  two harness checks and failed the import before the compatibility fix.
- Regression coverage: The shared default policy permits at most 10,000 entries,
  256 MiB per entry, 1 GiB aggregate output, 1 GiB compressed input, 64 MiB of
  central-directory metadata, and a 512:1 expansion ratio. Tests cover each
  budget, truncated and trailing GZIP streams, CRC and declared-size mismatch,
  stored and deflated ZIP members, standard data-descriptor members, DOS file
  attributes, encrypted members, malformed local headers, destination rollback,
  symlinks, and path traversal. Cloud and local activity imports verify that
  rejected payloads never reach a parser. Backup tests preserve valid large and
  highly compressible archives.
- Resolution: GZIP and ZIP extraction now stream in 64 KiB chunks through
  bounded devices instead of materializing untrusted output in memory. GZIP
  validates its trailer CRC and size and rejects concatenated or trailing data.
  ZIP scans the central directory under explicit budgets, validates local and
  central metadata, sizes, payload bounds, and CRCs while supporting standard
  data descriptors, then stages every selected member in a temporary directory
  before committing with `QSaveFile`. A shared compressed-activity helper
  accepts exactly one non-directory, non-symlink ZIP member, including DOS ZIP
  files without Unix mode bits, and writes imports to random temporary files.
  Self-generated athlete backups derive trusted bounds from their manifest and
  archive size so the generic bomb limits do not break compatible backups.
- Verification: The final archive, migration, and backup suites passed 46, 102,
  and 17 tests normally and under strict ASan/UBSan/LSan, with no sanitizer
  reports. A fresh full release build and all 63 unit-test projects passed
  (2,052 passed, 0 failed, 0 skipped, 0 blacklisted). The 165,534,200-byte
  AppImage reports `V3.8-DEV2605 (5012)` and has SHA-256
  `b4ea2d5da5114e12d87bbad0355752dc7f2cbdfb5e81cf061bda23f587ae6027`.
  It remained stable for a 15-second clean-profile X11 launch and a 45-second
  launch against a copied real athlete profile. No linker, WebEngine, migration,
  or crash error remained; logs contained only known missing translator,
  optional configuration, Bluetooth capability, and OpenData-secret notices.

### PARSE-002: CP CSV gaps can expand to billions of points

- Status: FIXED
- Code: `src/FileIO/CsvRideFile.cpp`, `src/FileIO/CpCsvImport.cpp`,
  `src/FileIO/CpCsvImport.h`, `src/src.pro`,
  `unittests/FileIO/cpCsvImport/cpCsvImport.pro`,
  `unittests/FileIO/cpCsvImport/testCpCsvImport.cpp`,
  `unittests/unittests.pro`
- Impact: One attacker-controlled timestamp can consume CPU and memory for a
  billion-iteration expansion.
- Test-first evidence: The new focused test target was added before the
  production helper and initially failed to configure and build because the
  bounded CP import implementation did not exist. The first strict ordered
  implementation then passed 21 tests and failed three compatibility
  regressions for non-monotonic rows and duplicate timestamps. Those failures
  reproduced ordering used by GoldenCheetah's own filtered and rainbow CP
  exports before normalization was implemented.
- Regression coverage: The 24 focused tests cover representative sparse curves,
  reconstructed cumulative averages, the optional model column, a
  billion-second timestamp, negative, zero, fractional, non-numeric and
  non-finite timestamps, non-monotonic rows, matching and conflicting duplicate
  timestamps, invalid power values, whole-file row and expanded-point budgets,
  late rollback, and empty input.
- Resolution: CP rows are now parsed into a compact builder under a 345,600-row
  budget and a two-day/172,800-point duration budget. Timestamps and values must
  be finite, timestamps must be positive whole seconds, and power must be
  non-negative. Finalization stably orders bounded input, coalesces exact
  duplicates, rejects conflicts, and derives compact power segments using
  checked extended-precision arithmetic. The complete curve is validated before
  `RideFile` is mutated; only the final, bounded points are then emitted in
  ascending order, with no second per-point staging container.
- Verification: The focused suite passed all 24 tests normally and under strict
  ASan/UBSan/LSan, with no sanitizer reports. A fresh `-O2` release build and all
  64 unit-test projects passed (2,076 passed, 0 failed, 0 skipped, 0
  blacklisted). The 165,640,696-byte AppImage reports `V3.8-DEV2605 (5012)` and
  has SHA-256
  `6ffe67dcd1bbdabcf7ee4e417afbabc5cf3b9eef9e5c625440cb4c100191e4b3`.
  It remained stable for a 15-second clean-profile X11 launch and a 45-second
  launch against a copied real athlete profile; logs contained only missing
  translator debug notices. A separate direct AppImage run with a disposable
  copied profile loaded an existing activity and rendered its metrics dashboard;
  before/after metadata and settings hashes confirmed that the live profile was
  unchanged. The host screen lock prevented reliable click-through automation,
  so this verification does not claim an interactive workflow.

### PARSE-003: TCX swim gaps amplify into thousands of points per record

- Status: FIXED
- Code: `src/FileIO/TcxParser.cpp`, `src/FileIO/TcxParser.h`,
  `src/FileIO/TcxRideFile.cpp`, `unittests/FileIO/tcxPointBudget/`, and
  `unittests/unittests.pro`
- Impact: Repeated swim gaps and pause laps expanded independently into
  thousands of synthetic points. There was no aggregate output or rewrite-work
  budget, malformed negative pauses could move the swim cursor backward and
  make later rewrites quadratic, and a late failure could publish earlier
  activities from the same file. Capped and fractional expansions also dropped
  the real source endpoint, while consecutive pauses reused a stale trackpoint
  timestamp.
- Test-first evidence: The focused target first established ordinary swim and
  non-swim compatibility plus whole-file rollback. Before the final invariant
  fix, the expanded RED run passed 9 cases and failed 6: capped gap and pause
  imports produced 7,501 rather than 7,502 points, a fractional gap produced 6
  rather than 7, consecutive pauses stopped at second 3 rather than 6, a
  negative pause emitted a negative SWIM duration, and a corrupt high-water
  mark produced one point rather than six.
- Regression coverage: Fifteen behavioral cases cover ordinary interpolation,
  no-list multi-activity ownership, aggregate and exact 172,800-point budget
  boundaries, capped and fractional endpoints, capped and consecutive pauses,
  repeated negative pauses, corrupt settings, disabled smart recording, sparse
  GPS activities, hostile gap and pause amplification, and atomic rollback.
- Resolution: TCX import now reserves every generated batch against a
  172,800-point whole-file budget and bounds swim rewrite work to 345,600
  visited points. The Garmin high-water mark is normalized to 1-300 seconds,
  per-event expansion remains bounded, and capped or fractional data retains a
  sparse real endpoint. Invalid or non-increasing sample time cannot regress
  parser state; pause expansion is anchored to the Lap timeline and keeps both
  output and swim cursors monotonic. Parsed activities remain staged until the
  complete bounded import succeeds, and all staged rides and pending SWIM data
  are released on rollback or when extra activities are not requested.
- Verification: The focused suite passes 17/17 both normally and under strict
  ASan/UBSan/LSan, with no sanitizer report. The related interval-ownership and
  split suites pass 8/8 and 13/13 under the same sanitizers. A fresh `-O2`
  release build and all 65 unit-test suites pass 2,096 tests (0 failed, 0
  skipped, 0 blacklisted). The 166,500,856-byte AppImage reports
  `V3.8-DEV2605 (5012)` and has SHA-256
  `87ef807d7d5f43c8e728e8686729991de9c8eebe537d53b0224d1017812bca8b`.
  It remained stable for a 15-second clean-profile X11 launch and a 45-second
  launch against a copied real athlete profile; logs contained only missing
  translator debug notices.

### PARSE-004: Malformed XML is accepted as a partial activity

- Status: FIXED
- Code: `src/FileIO/TcxRideFile.cpp`, `src/FileIO/GpxRideFile.cpp`,
  `src/FileIO/FitlogRideFile.cpp`, `unittests/FileIO/xmlImportIntegrity/`, and
  `unittests/unittests.pro`
- Impact: TCX, GPX, and Fitlog ignored `QXmlSimpleReader::parse()` failure after
  parser callbacks had already populated an activity. A malformed tail could
  therefore return partial data as valid. Fitlog also published activities to
  the caller's list before the complete document was known to be valid, and a
  valid multi-activity import leaked unrequested extra rides when no list was
  supplied.
- Test-first evidence: The new focused target initially passed 9 cases and
  failed 63. Every truncated or otherwise malformed document returned partial
  data, malformed TCX and Fitlog multi-activity imports published partial
  state, and LSan reported a 3,566-byte leak in 23 allocations for an
  unrequested second Fitlog activity.
- Regression coverage: The 72-test focused target imports valid minimal TCX,
  GPX, and Fitlog files; truncates each format at every XML element boundary;
  adds incomplete trailing markup and mismatched roots; verifies caller-list
  atomicity; preserves valid multi-activity ordering; and exercises imports
  that do not request extra activities.
- Resolution: All three readers now require a successful complete XML parse
  before returning an activity. TCX and Fitlog stage every parsed ride locally,
  release all staged state on failure, and publish to the caller only after
  success. Fitlog also releases successful extra activities that were not
  requested. TCX retains the more specific aggregate point-budget error when
  both the parser limit and XML parse result indicate failure.
- Verification: The focused suite passes 72/72 both normally and under strict
  ASan/UBSan/LSan, with no sanitizer report. The related TCX point-budget suite
  passes 17/17 in both configurations. A fresh `-O2` production build and all
  66 unit-test suites pass 2,168 tests (0 failed, 0 skipped, 0 blacklisted).
  The 166,259,192-byte AppImage reports `V3.8-DEV2605 (5012)`, has SHA-256
  `9dd770ee212fcd8e3f5adf2547602dd926c9061b7aa4885e7a72e041d11347c3`,
  and remained stable for 15-second direct X11 and display-free offscreen
  launches with clean disposable profiles; both logs contained only missing
  C-locale translator debug notices.

### PARSE-005: JSON parser checks the wrong error list

- Status: FIXED
- Code: `src/FileIO/JsonRideFile.y`,
  `unittests/FileIO/jsonImportIntegrity/`, and
  `unittests/unittests.pro`
- Impact: The reader ignored the parser return value and then checked the
  caller's pre-existing `errors` list instead of
  `JsonContext::JsonRideFileerrors`. Malformed JSON could therefore return a
  partially populated activity as valid, while a valid activity was rejected
  if the caller already had an unrelated diagnostic.
- Test-first evidence: The new target was run against `c28a123`, the parent
  of the existing parser correction `d33e72fe`. It passed 3 cases and failed
  123: all 122 malformed documents returned partial activities, and a valid
  document with a pre-existing caller diagnostic was rejected.
- Regression coverage: The target imports a complete document, truncates it
  after 108 structural boundaries and before each of eight major sections,
  and covers empty input, trailing data, a mismatched root, invalid UTF-8, and
  caller-error preservation.
- Resolution: The parser correction in `d33e72fe` requires a successful
  parser return and an empty parser-owned error list, appends those actual
  errors to the caller, and destroys partial state before returning null.
- Verification: `jsonImportIntegrity` passes 126/126 both normally and under
  strict ASan/UBSan/LSan, with no sanitizer report. The related
  `rideFileOwnership` suite passes 10/10 in both configurations. A fresh `-O2`
  release build and all 67 unit-test suites pass 2,296 tests (0 failed, 0
  skipped, 0 blacklisted).

### PARSE-006: FIT integrity and truncation checks are incomplete

- Status: FIXED
- Code: `src/FileIO/FitFileIntegrity.cpp`,
  `src/FileIO/FitFileIntegrity.h`, and `src/FileIO/FitRideFile.cpp`
- Impact: Length narrowing, ignored CRCs, and partial recovery can accept corrupt
  files and inconsistent data.
- Regression evidence: The test-first `fitImportIntegrity` target initially failed
  because the integrity implementation did not exist. Its independent FIT encoder
  and CRC implementation then exercised valid 12- and 14-byte headers, optional
  zero header CRCs, chained files, malformed signatures, wrong declared lengths,
  invalid header and file CRCs, trailing bytes, every truncation boundary, closed
  devices, position restoration, record accounting, and the 512 MiB physical-size
  limit. `fitReaderIntegrity` drives the real parser through valid, corrupt,
  truncated, semantically overrun, and chained inputs and verifies that failures
  return no partial `RideFile`.
- Resolution: A streaming, overflow-safe validator now checks every chained FIT
  segment before parsing and restores the input position. The parser uses 64-bit
  record budgets, rejects physical or semantic overruns, reads all chained
  segments without the binary-unsafe `canReadLine()` gate, and keeps the ride and
  XData objects under RAII ownership until successful transfer.
- Verification: `fitImportIntegrity` passes 94/94 and `fitReaderIntegrity` passes
  8/8 in release and strict ASan/UBSan/LSan configurations. The integrity suite
  accepts all 41 repository FIT fixtures, including the three-segment Garmin HRM
  swim fixture. A fresh release build succeeds, the AppImage packaging check
  passes, and all 70 QtTest suites pass 2,431 tests (0 failed, 0 skipped, 0
  blacklisted) with the offscreen Qt platform.

### CLOUD-002: Strava OAuth can still report an authentication-required error

- Status: FIXED
- Code: `src/Cloud/OAuthDialog.cpp`, `src/Cloud/Strava.cpp`,
  `src/Cloud/StravaOAuthPolicy.cpp`,
  `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/MakeAppImageQt6.sh`, and
  `appveyor/linux/after_build.sh`
- Observed symptom: On 18 July 2026, a Strava connection attempt reported
  `Error retrieving access token, Host requires authentication (204)`.
- Root cause: `204` was Qt's
  `QNetworkReply::AuthenticationRequiredError` enum value, not an HTTP status.
  The failing custom executable contained the build placeholder as its client
  secret, so Strava correctly rejected the token exchange. The then-current
  production AppImage had SHA-256
  `8897d666c17b2d9a0a36df7492b70011459f7021e3978c6547ef013bec39f1ca`;
  its build metadata identified commit `b30d8de` and explicitly reported
  `Strava OAuth: unavailable (credentials not configured)`.
- Provider evidence: The current official Strava authentication documentation
  still specifies `POST https://www.strava.com/oauth/token` with form-encoded
  client ID, client secret, authorization code or refresh token, and grant
  type. A controlled request using a configured upstream release credential
  and an intentionally invalid authorization code returned HTTP 400 with a
  `code` error and no client-credential error. This proves the endpoint,
  request form, and application credential remain accepted; the failure was
  local packaging rather than a Strava protocol or subscription change.
- Regression evidence: The packaging test first failed because the release
  credential gate did not exist. A second RED case reproduced the false
  `configured` result when a compressed Type 2 AppImage was inspected as if it
  were a raw executable. A third RED case showed that the development-container
  example generated numeric macros instead of C++ string literals.
- Resolution: Both production AppImage packagers now refuse to package a raw
  GoldenCheetah executable unless Strava OAuth is configured. Status inspection
  rejects compressed AppImages and requires their extracted executable instead.
  The development-container example uses qmake's required escaped quoting.
  Build-only credentials remain in the git-ignored local `gcconfig.pri` with
  owner-only permissions and are excluded from repository diffs and history.
- Verification: The packaging regression test and shell syntax checks pass.
  `stravaOAuthPolicy` passes 32/32 focused tests. A fresh configured release
  build passes its credential gate, and all 70 QtTest suites pass 2,431 tests
  (0 failed, 0 skipped, 0 blacklisted); the AppImage packaging test also passes.

### BUILD-005: Production AppImages allow missing Strava credentials

- Status: FIXED
- Code: `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/MakeAppImageQt6.sh`,
  `appveyor/linux/after_build.sh`, and
  `unittests/Build/appImagePackaging/testAppImagePackaging.sh`
- Impact: Packaging completed successfully with the placeholder client secret,
  producing a release in which Strava authorization could only fail.
- Test-first evidence: The new release-gate assertion failed because no
  `require_strava_oauth_build` helper existed and neither packager called one.
- Resolution: A shared release gate accepts only the exact configured status,
  and both production packagers invoke it before creating an AppDir.
- Verification: ASCII and Qt UTF-16LE placeholders, missing executables, and
  configured executables are covered; the focused packaging test and full
  test matrix pass.

### BUILD-006: Compressed AppImage status has a false configured result

- Status: FIXED
- Code: `src/Resources/linux/AppImagePackagingSupport.sh` and
  `unittests/Build/appImagePackaging/testAppImagePackaging.sh`
- Impact: Searching the compressed squashfs payload for a raw placeholder
  usually found nothing and incorrectly reported `Strava OAuth: configured`.
- Test-first evidence: A Type 2 AppImage-magic fixture was accepted as a raw
  configured executable before the correction.
- Resolution: Status inspection detects the `AI\002` Type 2 marker and fails
  with an instruction to inspect the extracted GoldenCheetah executable.
- Verification: The compressed fixture is rejected, a configured raw fixture is
  accepted, and both real extracted configured and placeholder executables are
  classified correctly.

### BUILD-007: Development Strava defines lose C++ string quoting

- Status: FIXED
- Code: `.devcontainer/gcconfig.pri` and
  `unittests/Build/appImagePackaging/testAppImagePackaging.sh`
- Impact: Following the example produced numeric preprocessor values. Calls to
  `QStringLiteral(GC_STRAVA_CLIENT_ID)` and
  `QStringLiteral(GC_STRAVA_CLIENT_SECRET)` then failed to compile.
- Test-first evidence: The packaging test failed when it required the same
  triple-escaped qmake form already documented by `src/gcconfig.pri.in`.
- Resolution: Both example defines now preserve quoted C++ string literals
  through qmake and the generated Makefile.
- Verification: A clean release build reports quoted, non-placeholder defines
  and compiles successfully; the focused and full test suites pass.

### DUR-005: QSettings migration is not resumable after partial success

- Status: FIXED
- Code: `src/Core/Settings.cpp`, `src/Core/Settings.h`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: Any partially populated new settings file suppresses remaining legacy
  migration, making configuration and credentials appear lost.
- Test-first evidence: The original implementation passed 77 focused cases and
  failed 24 restart cases. Follow-up RED cases reproduced normal startup font
  writes suppressing system migration, one-time `started` sync failures being
  lost to later application writes, a `SystemScope` fallback suppressing the
  exact user-file migration, constructor-only disk mutation, and a temporary
  regression in the public fallback-aware `contains()` contract.
- Resolution: System, global, and athlete scopes now use versioned
  `started`/`complete` states. A system state is prepared without constructor
  I/O and persisted before the first real system write. Each allowlisted value
  is copied only when its exact destination file lacks the key, preserving
  empty strings, false, and zero. Global-to-system and athlete-to-global writes
  participate in checked synchronization, and `complete` is persisted only
  after every target reports `QSettings::NoError`. A failed `started` write
  remains in the in-memory settings map so a later successful application sync
  cannot publish a markerless partial target. Migration target checks ignore
  Qt fallback locations while the public runtime API retains normal fallback
  semantics. Credential vault sweeps remain outside the legacy marker so they
  continue retrying independently.
- Verification: The 105-case focused suite passes normally and under strict
  ASan/UBSan/LSan with leak detection. It covers all 15 scope participant and
  marker write failures, one-time failure followed by a same-process settings
  write, restart recovery, real startup ordering, exact fallback handling,
  marker rollout, unknown states, dynamic device/data-processor/column keys,
  athlete key renaming, splitter keys, and cross-store values. The production
  application compiles and links as a 536,217,336-byte ELF. The complete
  out-of-source matrix runs 2,819 cases across 81 QtTest programs: 2,817 pass,
  none fail, and two source-contract checks skip as tracked by `TEST-002`.
- Residual: A markerless partial profile created before this state protocol is
  indistinguishable from an established profile where the user deliberately
  removed old settings. Such profiles are conservatively adopted as complete
  to avoid resurrecting stale legacy values; automatic historical repair
  requires an explicit user-selected repair policy.

### DUR-006: RideCache background save lacks an immutable snapshot

- Status: OPEN
- Code: `src/Core/RideCache.cpp:673`, `src/Core/RideDB.y:481`,
  `src/Core/RideDB.y:369`
- Impact: Concurrent import/delete/metadata edits can race serialization and
  produce inconsistent or malformed cache JSON.
- Test: Mutate the cache during save under TSAN and validate every output file.
- Fix direction: Snapshot on the owning thread, serialize the snapshot to
  `QSaveFile` in the worker.

### DUR-007: Split transactions have no restart recovery journal

- Status: OPEN
- Code: `src/FileIO/AtomicFileWriter.h`,
  `src/Gui/SplitActivitySave.cpp`
- Impact: Runtime failures are rolled back, but a process or power loss between
  publishing outputs, preserving an old backup, and archiving the source can
  leave a recoverable mixture of split files and `.rollback-*` state with no
  automatic reconciliation on restart.
- Test: Run each durable transition in a subprocess, terminate it at injected
  failpoints, restart, and require deterministic completion or rollback without
  losing the source or a prior backup.
- Fix direction: Use a private transaction directory and fsynced manifest with
  explicit states, then reconcile incomplete transactions before loading the
  activity cache.

### DUR-008: Staged-set rollback trusts a mutable target pathname

- Status: OPEN
- Code: `src/FileIO/AtomicFileWriter.h:697`
- Impact: GoldenCheetah holds cooperative path locks, but another process can
  replace a newly published target before finalization fails. Rollback removes
  the current pathname and could therefore delete the other process's file.
- Test: Replace a published target through an injected non-cooperating writer
  during finalization and verify rollback removes only the exact file identity
  created by this transaction.
- Fix direction: Record and revalidate platform file identities or publish an
  immutable generation and atomically switch one manifest instead of deleting
  rollback targets by pathname.

### DB-001: VideoSync import uses video-table helpers

- Status: OPEN
- Code: `src/Train/TrainDB.cpp:1065`
- Impact: Replace can delete a same-path video and update can skip an existing
  videosync row.
- Test: Cover insert/update/replace with identical paths in both tables.
- Fix direction: Use videosync helpers or one SQLite upsert.

### DB-002: Workout update does not update average power

- Status: OPEN
- Code: `src/Train/TrainDB.cpp:1027`
- Impact: Edited workouts retain stale `erg_avg_power` metadata.
- Test: Insert, change power, update, and query the stored average.
- Fix direction: Assign the bound `:erg_avg_power` value.

### DB-003: Training-library transaction failures are ignored

- Status: OPEN
- Code: `src/Train/TrainDB.cpp:795`,
  `src/Train/TrainDB.cpp:803`, `src/Train/Library.cpp:144`
- Impact: Partial imports can be reported to the UI as successful.
- Test: Force duplicate, schema, and commit failures and require rollback.
- Fix direction: RAII transaction with propagated result and post-commit signals.

### TRN-004: Core-temperature header is written to the RR file

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:3486`
- Impact: TCR lacks its header and RR can be corrupted by a TCR header.
- Test: Round-trip core and RR data both together and independently.
- Fix direction: Construct the header stream on `tcoreFile`.

### TRN-005: Discard leaves auxiliary recording files behind

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:1663`,
  `src/Train/TrainSidebar.cpp:1701`
- Impact: `.rr`, `.pos.csv`, `.vo2`, and `.tcr` files remain orphaned.
- Test: Create every sidecar, discard, and require all artifacts removed.
- Fix direction: Track and dispose the complete recording artifact set.

### TRN-006: Initial start signal is emitted twice

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:1408`,
  `src/Train/TrainSidebar.cpp:1414`, `src/Train/VideoWindow.cpp:346`
- Impact: Consumers reset twice and the first callback observes non-running state.
- Test: `QSignalSpy` must observe exactly one start after complete initialization.
- Fix direction: Set state/timers first and emit once.

### TRN-007: First workout target is delayed by the load timer

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:1389`,
  `src/Train/TrainSidebar.cpp:1438`, `src/Train/TrainSidebar.cpp:2432`
- Impact: The trainer can retain its previous target for roughly one second.
- Test: A fake controller must receive the zero-time target before event-loop
  advancement.
- Fix direction: Calculate and apply the initial target synchronously.

### DEV-005: Daum restart leaves the trainer paused

- Status: OPEN
- Code: `src/Train/Daum.cpp:45`, `src/Train/Daum.cpp:50`
- Impact: Both pause and restart set `paused_ = true`, preventing later load writes.
- Test: State-machine test for start, pause, restart, and stop.
- Fix direction: Set `paused_ = false` in restart.

### METRIC-001: Missing/cyclic metric dependencies can loop forever

- Status: OPEN
- Code: `src/Metrics/RideMetric.cpp:226`,
  `src/Metrics/RideMetric.cpp:242`, `src/Metrics/RideMetric.cpp:281`
- Impact: Refresh workers repeatedly requeue an unresolvable parent metric.
- Test: Missing, self-cycle, multi-node cycle, diamond, and valid graphs.
- Fix direction: Validate and topologically order the dependency graph.

### METRIC-002: User metrics retain the first athlete Context

- Status: OPEN
- Code: `src/Core/RideCache.cpp:77`, `src/Metrics/UserMetric.cpp:27`,
  `src/Core/Context.cpp:134`
- Impact: Closing the first athlete can leave global metrics with a dangling
  context while other athletes remain open.
- Test: Open two athletes, close the first, and evaluate/reload metrics under ASan.
- Fix direction: Compile formulas context-free and pass athlete services at
  evaluation time.

### METRIC-003: Global metric reload races other athlete workers

- Status: OPEN
- Code: `src/Gui/ConfigDialog.cpp:241`,
  `src/Core/Context.cpp:130`, `src/Core/RideCache.cpp:743`
- Impact: One athlete cancels only its own cache before global metric objects are
  removed while other workers may still use them.
- Test: Multi-athlete metric reload during refresh under TSAN.
- Fix direction: Publish an immutable registry snapshot under one lock.

### GUI-001: RideNavigator stores a dangling stack address in QModelIndex

- Status: OPEN
- Code: `src/Gui/RideNavigatorProxy.h:243`
- Impact: `mapFromSource` stores `&p`, the address of a local pointer, as
  `internalPointer`; later mapping dereferences invalid stack memory. The heap
  allocated QModelIndex is also leaked and source row zero is excluded.
- Test: Round-trip every source/proxy row under ASan, including row zero and
  model resets.
- Fix direction: Use stable model-owned identity/internal IDs without heap or
  stack pointer storage.

### GUI-002: Ride deletion can retain a deleted current selection

- Status: OPEN
- Code: `src/Core/RideCache.cpp:377`, `src/Core/RideCache.cpp:439`,
  `src/Core/Context.h:257`
- Impact: Deleting the final/current ride can leave the deleted object selected;
  some non-current deletions also omit the deletion signal.
- Test: Delete first, middle, final, current, and non-current rides and verify
  signal order plus a valid/null selection.
- Fix direction: Formalize about-to-remove, removal, selection, deleted, selected
  ordering and prevent notifier side effects.

### GUI-003: Power histogram selection guard is inverted

- Status: OPEN
- Code: `src/Charts/PowerHist.cpp:2401`
- Impact: The `RideFilePoint*` overload enters its interval loop only when
  `rideItem` is null and then dereferences it. A null item can therefore
  crash, while every normal non-null ride skips the loop and reports all point
  samples as unselected. Selected intervals are missing from the standard
  power histogram even though the time-based W' balance overload is correct.
- Evidence: GCC 13 diagnoses that the loop calls a member function through a
  null `this` pointer. The adjacent time-based overload has the intended
  positive guard.
- Test: Exercise the point overload with no ride, a ride with no selected
  intervals, overlapping selected intervals, and exact sample boundaries.
- Fix direction: Return false for a null ride/point and iterate selected
  intervals only when `rideItem` is valid, matching the time-based overload.

### MAP-001: Map nearest-point longitude scaling uses degrees as radians

- Status: OPEN
- Code: `src/Charts/RideMapWindow.cpp:1772`
- Impact: `cos(latitude)` receives degrees, selecting the wrong route point at
  many latitudes.
- Test: Known routes at equatorial and high latitudes with expected nearest point.
- Fix direction: Convert latitude to radians or use a geodesic helper.

### MAP-002: Map mouse movement repeatedly scans the full activity

- Status: OPEN
- Code: `src/Charts/RideMapWindow.cpp:1758`
- Impact: Every mousemove performs an O(N) point search, causing stalls for
  long/high-frequency activities.
- Test: Benchmark hover on 10k/100k/1m point routes.
- Fix direction: Spatial index or map-rendered point/index identifiers.

### ARCH-001: Context is a cross-layer mutable service locator

- Status: OPEN
- Code: `src/Core/Context.h:22`, `src/Core/Context.h:147`,
  `src/Core/Context.cpp:154`
- Impact: Core, GUI, Train, FileIO, Cloud, and WebEngine lifetimes are coupled,
  making thread ownership and isolated tests difficult.
- Test: Architectural dependency check plus headless construction tests for
  extracted services.
- Fix direction: Incrementally introduce `AthleteSession`, `TrainingSession`,
  and narrow settings/persistence/application service interfaces.

### ARCH-002: Unit tests link private application object files

- Status: OPEN
- Code: `unittests/unittests.pri.in:8`, `unittests/unittests.pri.in:19`,
  `src/src.pro:41`
- Impact: Tests depend on build paths/configuration, compile as C++11 while the
  application uses C++17, and omit most parser/training registrations.
- Test: Build tests from a clean tree on every platform without prebuilt app
  object discovery.
- Fix direction: Extract Core/FileIO/Train library targets and link tests normally.

### CI-001: Pull-request CI does not execute unit tests

- Status: OPEN
- Code: `.github/workflows/ci.yml:28`, `.github/workflows/ci.yml:37`,
  `.github/scripts/build.sh:69`
- Impact: The CI "Test" step only invokes `--version`; parser, database, and
  platform regressions can merge despite the existing test suite.
- Test: CI self-check must fail if zero test cases are discovered.
- Fix direction: Linux/macOS/Windows matrix, build and run all tests, then add
  ASan/UBSan, TSAN where viable, and parser fuzzers.

### CI-002: Season parser test depends on its working directory

- Status: FIXED
- Code: `unittests/Core/seasonParser/testSeasonParser.cpp:105`
- Impact: A clean out-of-source release `make check` cannot find
  `seasons.xml`, so the otherwise passing suite stops at `readSeasons()` and
  does not execute the remaining tests.
- Regression test: The unmodified release test failed with zero parsed seasons
  when run from `build-thread004-release`; after the fix, the focused suite
  passes 8/8 and the complete registered release suite passes.
- Fix: Resolve the fixture with QtTest's `QFINDTESTDATA` and fail explicitly if
  the test data cannot be located.

### BUILD-001: Release dependencies and tooling are not reproducibly pinned

- Status: OPEN
- Code: `appveyor/linux/after_build.sh:36`,
  `appveyor/linux/install.sh:28`, `src/Python/requirements.txt:5`
- Impact: Moving tool/dependency targets can change or break artifacts for the
  same source commit.
- Test: Repeat the build from a locked manifest and compare dependency/SBOM data.
- Fix direction: Pin commits/digests, hash-lock Python dependencies, generate an
  SBOM, and smoke-test AppImage on the oldest supported glibc.

### BUILD-003: AppImage Python runtime is incompatible and its release asset moves

- Status: FIXED
- Code: `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/MakeAppImageQt6.sh`,
  `appveyor/linux/after_build.sh`,
  `unittests/Build/appImagePackaging/testAppImagePackaging.sh`
- Impact: The local packager embedded Python 3.7 even though the pinned SIP
  6.15.1 build dependency requires Python 3.10 or newer, so a clean AppImage
  package failed. The upstream `python3.11` release tag also replaced and
  deleted its patch-version asset between consecutive package attempts, making
  a previously successful source URL return 404. Exported source trees had no
  Git revision for the package metadata, and the development container did not
  provide every downloader or FUSE tool assumed by the script.
- Test-first evidence: The first full package failed because pip could not find
  a SIP 6.15.1 distribution compatible with Python 3.7. A subsequent clean
  package reproduced the moving-tag failure when the former Python 3.11.14
  asset disappeared. The shell regression test then failed until the shared
  helper pinned the final runtime, verified its digest, supported exported
  source revisions, and scoped extraction mode to packaging tools.
- Resolution: Both Linux packagers now share one helper that installs the
  project-controlled Python 3.11.15 AppImage from an immutable release URL,
  verifies SHA-256 before extraction, supports curl or wget, cleans the AppDir,
  and validates explicit source revisions when `.git` is absent. The generic
  `APPIMAGE_EXTRACT_AND_RUN` override applies only to packaging-tool AppImages,
  so the resulting GoldenCheetah image still launches directly.
- Verification: The registered packaging test passes in the full release
  matrix. A clean package completed with Python 3.11.15 and SIP 6.15.1; its
  281,557,496-byte AppImage has SHA-256
  `bfaf42a134e0ed6801f2c893ca18253f6b2c7ee66472a573e9e032681759a261`.
  Separate clean-profile direct X11 and offscreen launches remained running for
  their 15-second smoke windows with only translator debug notices.

### BUILD-004: Containerized AppImage validation assumes FUSE

- Status: FIXED
- Code: `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/MakeAppImageQt6.sh`,
  `unittests/Build/appImagePackaging/testAppImagePackaging.sh`
- Impact: AppImage generation can finish successfully and then be rejected by
  its own offscreen smoke test when the build container has no FUSE device or
  `fusermount` binary. This blocks otherwise valid containerized releases and
  encourages setting `APPIMAGE_EXTRACT_AND_RUN` globally, which would mask the
  direct-launch behavior that the release must still support on the host.
- Test-first evidence: Packaging commit `0012d7a` produced a 281,528,824-byte
  image, but its final smoke step exited 127 with `No suitable fusermount
  binary found`. A shell regression then failed because no packaged-image
  smoke helper existed; its synthetic executable requires extraction mode to
  be present in the child environment. After that smoke passed, the next full
  package exposed a second direct launch: version metadata collection failed
  with the same exit 127. A source regression failed until that invocation was
  routed through a scoped extraction helper too.
- Resolution: A shared helper scopes `APPIMAGE_EXTRACT_AND_RUN=1` to the
  timeout-controlled packaged-image smoke process. Packaging tools retain their
  separate scoped helper, which now also handles packaged-image version
  metadata. Normal host launches receive no global override.
- Verification: The shell regression and `bash -n` pass. The complete matrix
  still passes 68 QtTest suites and 2,328 tests with no failures, skips, or
  blacklists. The generated image remained running for the expected ten-second
  offscreen window in the same FUSE-less container and returned the accepted
  timeout status, and its version query then exited zero through the scoped
  helper. The logs contained only PipeWire and locale notices.

### CLOUD-001: Local releases advertise Strava OAuth with placeholder credentials

- Status: FIXED
- Code: `src/Cloud/StravaOAuthPolicy.cpp`,
  `src/Cloud/OAuthDialog.cpp`, `src/Cloud/Strava.cpp`,
  `src/Cloud/AddCloudWizard.cpp`,
  `src/Resources/linux/AppImagePackagingSupport.sh`
- Observed symptom: Connecting Strava in the BUILD-003 AppImage ends with
  `Error retrieving access token, Host requires authentication (204)` after
  the user grants access in the browser.
- Impact: Development and local release builds compile the literal
  `__GC_STRAVA_CLIENT_SECRET__` fallback while still presenting Strava as an
  available service. They submit that placeholder with client ID 83, so Strava
  rejects the authorization-code exchange. The dialog then prints Qt network
  enum value 204 without the HTTP status or sanitized provider response,
  making the failure look like an HTTP 204 and hiding the actionable cause.
- Evidence: The BUILD-003 executable contains the exact placeholder and the
  configured client ID. The official 8 July 2026 upstream snapshot contains
  the same client ID but not the placeholder, while this fork has no Actions
  secret configured. Qt documents 204 as
  `QNetworkReply::AuthenticationRequiredError`; it is not the HTTP response
  status. Strava's current authentication documentation still requires a
  registered client ID and client secret for both token exchange and refresh,
  and its status page reports the API operational.
- Test-first evidence: The first focused build failed because the required
  Strava OAuth policy did not exist. After the initial helper passed its
  round-trip cases, a stricter raw-body test failed 30/31 and proved that
  `QUrlQuery` left `+` ambiguous in form data; the replacement percent-encodes
  each key and value explicitly. A provider-text boundary test then failed
  31/32 and proved that truncation before redaction could expose the beginning
  of a long reflected secret. Finally, the package inspector misclassified the
  newly linked placeholder build because `QStringLiteral` stored the marker as
  UTF-16LE; a synthetic Qt-literal shell case reproduced that failure before
  the inspector was changed.
- Resolution: One side-effect-free policy now validates credentials, builds
  authorization-code and refresh forms with every documented grant parameter,
  bounds and validates token responses, and formats HTTP/provider failures
  while redacting request secrets. Placeholder builds stop before opening the
  browser or sending a request and display a build-configuration error. Both
  token paths use the same policy, no longer log raw token responses, and only
  persist a complete access/refresh pair. AppImage packaging reports Strava as
  configured or unavailable without exposing credential values, recognizes
  both native and Qt string-literal marker encodings, and fails closed if it
  cannot perform both checks.
- Verification: All 32 focused cases pass normally and under strict
  ASan/UBSan/LSan. The application compiles and links with the production
  `OAuthDialog`, `Strava`, and wizard paths. The complete release matrix passes
  68 suites and 2,328 tests with no failures, skips, or blacklists. The package
  status helper identifies the local placeholder binary as unavailable and the
  official upstream snapshot as configured.
- External requirement: A working custom Strava integration still requires a
  legitimate private application secret supplied outside the public tree. The
  fix deliberately does not recover or publish the credential embedded in an
  official binary.

### CLOUD-004: Strava authorization ignores the scopes actually granted

- Status: FIXED
- Code: `src/Cloud/OAuthDialog.cpp:137`,
  `src/Cloud/OAuthDialog.cpp:576`, and
  `src/Cloud/StravaOAuthPolicy.cpp:240`
- Impact: GoldenCheetah requests read, private-activity read, and activity-write
  scopes but stores the tokens and reports full success without checking what
  the athlete granted. The connection can therefore appear configured while
  route, private-activity, upload, or stream operations predictably fail.
- Evidence: Strava permits athletes to deselect requested scopes. Since 23
  April 2026 its token response explicitly contains the granted scopes, while
  the local token parser reads only the access and refresh tokens.
- Test: Cover missing, empty, duplicate, unknown, and reordered space-delimited
  scope responses, plus a callback that reports a narrower scope. The result
  must either reject a connection missing required capabilities or persist the
  granted set and disable only unsupported operations with a clear warning.
- Fix direction: Parse scopes into a normalized bounded set, define the minimum
  set per advertised capability, persist the granted set, and make success and
  feature availability reflect that set.
- Test-first evidence: Commit `321519d` added authorization-scope cases before
  production support. Its focused build failed at compile time because
  `parseAuthorizationResponse()` and a granted-scope result did not exist.
  The cases cover missing, null, array, empty, blank, wrong-case, tab-delimited,
  oversized, excessive, and each individually missing required permission,
  plus reordered, duplicate, and unknown future scopes.
- Resolution: Authorization-code responses now use a dedicated parser while
  refresh responses retain their scope-free format. Scope input is bounded to
  2 KiB, 64 entries, and 128 bytes per RFC-compatible token, normalized to a
  sorted unique set, and compared case-sensitively with the exact three scopes
  GoldenCheetah requests. Unknown extra scopes remain forward-compatible.
  Missing or malformed grants clear both tokens and report the missing
  permissions before any credential publication. Since the current Strava
  provider advertises upload, download, query, and route behavior as one
  service, partial grants are rejected instead of presenting unsupported
  operations as available.
- Verification: All 47 focused OAuth, token, scope, and production-wiring cases
  pass normally and under strict ASan/UBSan/LSan. The complete application
  compiles and links, and the full normal matrix passes 75 suites and 2,530
  tests with no failures, skips, or blacklists.

### CLOUD-005: Interactive Strava token exchange has no timeout or reply cleanup

- Status: FIXED
- Code: `src/Cloud/OAuthDialog.cpp:484`,
  `src/Cloud/OAuthDialog.cpp:514`,
  `src/Cloud/OAuthDialog.cpp:546`, and
  `src/Cloud/OAuthTokenReplyController.cpp:15`
- Impact: An interactive authorization reply that never finishes can leave the
  dialog permanently pending. Completed replies remain manager children until
  the dialog is destroyed, which complicates every early error path.
- Evidence: The interactive token request starts no deadline and retains the
  raw reply through a manager-wide `finished` signal. The refresh half was
  resolved with THREAD-007: `Strava::open()` now uses a 30-second
  interruption-aware wait and schedules its reply for deletion on every exit.
- Test: Drive the interactive exchange with a reply that never emits
  `finished()`, explicit dialog cancellation, and repeated successful and
  failed replies. Each request must abort on its deadline or cancellation and
  leave no reply children after deferred deletes are processed.
- Fix direction: Give the interactive exchange an owned single-shot deadline
  and one cleanup path that disconnects, aborts when needed, and schedules the
  reply for deletion before accepting or rejecting the dialog.
- Test-first evidence: Commit `82a948b` added a dedicated controller suite
  before the implementation. Its qmake step found the project, then `make`
  failed because `OAuthTokenReplyController.{h,cpp}` did not exist. The tests
  cover invalid starts, normal completion, a real timer deadline, explicit
  cancellation, overlapping requests, untracked replies, and 100 sequential
  replies. The first GREEN run also exposed and removed a test-side
  use-after-delete instead of weakening deferred-deletion coverage.
- Resolution: Every interactive token POST now enters one reply controller
  with a 30-second deadline. Timeout and user cancellation abort only the
  tracked reply and remain distinguishable; cancellation closes silently while
  timeout reports its own error. Completion stops the timer and schedules every
  tracked or untracked reply for deletion. Manager completion is queued so an
  abort cannot re-enter modal UI or deferred deletion from inside the reply's
  call stack. Start failure and dialog destruction clear sensitive request
  context and fail closed. The same bounded lifecycle also protects the other
  OAuth providers sharing this dialog. The refresh half remains covered by the
  bounded `Strava::open()` path from THREAD-007.
- Verification: All 9 reply-controller cases and 47 Strava
  OAuth/policy/production-wiring cases pass normally and under strict
  ASan/UBSan/LSan. The complete application and updated test topology compile
  and link, and the full normal matrix passes 76 suites and 2,539 tests with no
  failures, skips, or blacklists.

### CLOUD-006: Strava Routes bypasses token refresh and blocks indefinitely

- Status: FIXED
- Code: `src/Cloud/Strava.cpp`, `src/Cloud/StravaAuthenticatedSession.cpp`,
  `src/Cloud/StravaNetworkReply.cpp`, `src/Cloud/StravaTokenRefresh.cpp`,
  `src/Train/StravaRoutesClient.cpp`, and
  `src/Train/StravaRoutesDownload.cpp`
- Impact: Opening the Routes dialog immediately reads the stored access token
  and performs athlete, route-list, and GPX requests without refreshing it.
  Strava access tokens expire after six hours, so the feature fails
  deterministically unless another Strava operation refreshed recently. Every
  request also waits in an unbounded nested event loop.
- Evidence: The route path reads `GC_STRAVA_TOKEN` directly and never invokes
  `Strava::open()` or a shared token provider. Its 401 path is reduced to an
  imprecise user-ID or network error.
- Resolution: Routes now uses a configured `Strava` service and a private
  authenticated session. The session obtains and durably publishes a grant
  before its first request, snapshots the token used by each attempt, and
  performs exactly one coordinated refresh and retry after HTTP 401. A 403 is
  reported without token rotation. The access token never crosses into Train
  or UI code. Real replies have a 30-second hard deadline, cancellation,
  bounded payload collection, guarded destruction, and one cleanup path.
  Athlete and route JSON, content types, pagination, 64-bit `id_str` values,
  duplicate IDs, GPX media types, XML structure, DTDs, and payload sizes are
  validated before use. The dialog defers its initial request until its modal
  event loop is active, stores IDs in `Qt::UserRole`, stages GPX in a private
  `QTemporaryFile`, commits final files with `QSaveFile`, and holds a database
  transaction only while importing.
- Verification: The RED cases reproduced stale-token use, duplicate refreshes,
  unbounded waits, token leakage, malformed and oversized responses, unsafe
  reply destruction, and reentrant token replacement. The final focused suites
  pass 21 token-refresh, 25 authenticated-session, 47 OAuth-policy, and 31
  Routes cases normally and under strict ASan/UBSan/LSan where applicable. The
  related 106-case athlete/cloud-lifecycle sanitizer suite also passes. The
  Qt 6.8.3 application compiles and links, and the complete release matrix
  passes 78 suites and 2,600 tests with no failures, skips, or blacklists.

### MEM-020: Network reply wait can outlive and dereference its reply

- Status: FIXED
- Code: `src/Cloud/NetworkReplyWait.cpp`, `src/Cloud/Strava.cpp`,
  `src/Cloud/Nolio.cpp`, `src/Cloud/OAuthPKCE.cpp`,
  `src/Cloud/WithingsDownload.cpp`, and
  `src/Cloud/TredictMeasuresDownload.cpp`
- Impact: If a pending reply or its manager is destroyed inside the nested
  event loop, the timeout and interruption callbacks dereference a freed raw
  pointer. Even after those callbacks were guarded, the wait returned
  `Finished`, allowing token-refresh callers to read the destroyed reply.
- Evidence: The RED controlled-reply test deleted a pending reply on the next
  event-loop turn. The original code crashed in the timeout callback after
  about one second. A second source-contract test showed that Strava refresh
  did not distinguish destruction from completion.
- Resolution: The wait owns a `QPointer`, quits on `destroyed`, guards every
  abort, and returns an explicit `Destroyed` result. All current callers check
  that result and their own guarded pointer before reading or deleting a reply.
  A throwing interruption callback is treated as cancellation.
- Verification: Destruction, timeout, and pre-existing interruption pass as
  direct contract tests. The complete 106-case athlete/cloud-lifecycle suite
  passes under strict ASan/UBSan/LSan, including Nolio, PKCE, Withings,
  Tredict, and Strava publication paths.

### SEC-014: Strava Routes stages private GPX in predictable files

- Status: FIXED
- Code: `src/Train/StravaRoutesClient.cpp` and
  `src/Train/StravaRoutesDownload.cpp`
- Impact: Route GPX can reveal precise home and travel locations. The old
  implementation used a deterministic temporary name with default file
  permissions and also included the provider-supplied route name in the final
  path. Concurrent local access, collisions, path separators, and stale files
  could expose data or redirect the write.
- Resolution: Route IDs are validated positive 64-bit decimal values and the
  final basename is derived only from that ID. Temporary GPX uses a random
  owner-private `QTemporaryFile` with automatic removal, and final replacement
  is atomic through `QSaveFile`.
- Verification: The filename suite rejects traversal input and requires a
  leaf-only result. Production wiring requires `QTemporaryFile` and rejects the
  former deterministic path. Valid and hostile GPX cases pass normally and
  under strict ASan/UBSan/LSan.

### SEC-015: Strava credentials can follow cross-origin HTTPS redirects

- Status: FIXED
- Code: `src/Cloud/OAuthDialog.cpp`, `src/Cloud/Strava.cpp`, and
  `src/Cloud/StravaAuthenticatedSession.cpp`
- Impact: Qt follows HTTPS-to-HTTPS redirects by default, including redirects
  to another host. Strava requests carry the client secret, refresh token, or
  bearer token, so accepting an unexpected cross-origin redirect can expose
  credentials. See the
  [Qt 6.5 redirect policy](https://doc.qt.io/qt-6.5/qnetworkrequest.html#RedirectPolicy-enum).
- Resolution: Every credential-bearing request in `Strava.cpp` and the
  interactive Strava token exchange uses `SameOriginRedirectPolicy`. The
  authenticated session also rejects every URL outside the current exact
  HTTPS API origin, default TLS port, and `/api/v3` path.
- Verification: Production wiring requires the policy on all six Strava
  request sites and the interactive exchange. Data-driven tests reject
  cleartext, foreign hosts, foreign ports, userinfo, OAuth paths, and API-prefix
  lookalikes before a grant or network operation can run.

### GUI-006: Strava Routes performs network work before the dialog is visible

- Status: FIXED
- Code: `src/Train/StravaRoutesDownload.cpp`
- Impact: The constructor immediately entered a nested network wait before
  `MainWindow` called `exec()`. The dialog was invisible, its cancellation
  controls were unavailable, and nested GUI events could re-enter the menu.
- Resolution: Initial refresh is posted with a context-bound zero-delay timer.
  It begins only after construction has returned and the modal event loop is
  active; destruction automatically cancels the callback.
- Verification: Production wiring requires the deferred timer and forbids the
  direct constructor call. The full application and complete matrix pass.

### THREAD-009: Reentrant Strava requests can refresh the wrong token

- Status: FIXED
- Code: `src/Cloud/StravaAuthenticatedSession.cpp`
- Impact: A nested event loop can install token B while a request made with
  token A is pending. The old response path reread the mutable member after a
  401 and could report B as rejected, rotate it unnecessarily, or fail to
  redact A from an error.
- Resolution: Each attempt snapshots its token for transport, rejection, and
  redaction. If a valid replacement was installed reentrantly, the session
  retries that replacement without invoking the refresh provider.
- Verification: The RED test installs token B from inside token A's request,
  returns 401, and requires requests A then B with only the initial grant call.
  All 25 session cases pass normally and under strict ASan/UBSan/LSan.

### THREAD-008: Strava refresh followers can still freeze the GUI

- Status: OPEN
- Code: `src/Cloud/StravaTokenRefresh.cpp` and
  `src/Train/StravaRoutesDownload.cpp`
- Impact: A Routes request on the GUI thread can join an in-flight refresh by
  waiting on a condition variable. Although the leader's real network request
  is bounded to 30 seconds, GUI events are not processed while the follower
  waits, so Close and Abort cannot update the cancellation flag during that
  interval.
- Test: Hold a worker refresh open, join it from the GUI through the Routes
  path, post Close, and require prompt cancellation without waiting for the
  leader's deadline.
- Fix direction: Expose asynchronous shared-flight completion or move the
  complete Routes request off the GUI thread. Do not solve this by pumping
  arbitrary nested GUI events inside the coordinator.

### PERF-008: Strava route imports no longer share one database transaction

- Status: OPEN
- Code: `src/Train/StravaRoutesDownload.cpp`
- Impact: Removing network waits from the original long transaction fixed lock
  duration, but each successfully downloaded route now starts and commits its
  own LUW. Importing many routes loses the previous batching benefit.
- Test: Import a representative multi-route set and compare transaction count
  and elapsed import time while proving no network wait occurs inside an LUW.
- Fix direction: Separate bounded download and validation from a short batched
  import phase, with bounded memory and correct partial-cancellation behavior.

### CLOUD-007: Removing Strava locally does not revoke provider authorization

- Status: FIXED
- Code: `src/Cloud/CloudService.h`, `src/Cloud/Strava.cpp`,
  `src/Cloud/StravaAccountRemoval.cpp`,
  `src/Cloud/StravaRevocationClient.cpp`,
  `src/Cloud/StravaCredentialPublisher.cpp`,
  `src/Cloud/StravaTokenPublication.cpp`,
  `src/Cloud/StravaTokenRefresh.cpp`, `src/Core/Settings.cpp`, and
  `src/Gui/AthletePages.cpp`
- Impact: Disabling or removing the local service leaves its access and refresh
  tokens usable and leaves GoldenCheetah authorized in the athlete's Strava
  account. Users can reasonably believe the integration was disconnected when
  only a local active flag changed.
- Test-first evidence: Commit `f35476c` added failing contracts for the absent
  revocation client, account-removal coordinator, generic disconnect API, and
  settings wiring before production implementation. Commit `12edd06` then
  captured the review-discovered fail-open cases: placeholder OAuth credentials
  mutating authorization state, uncertain refresh publication admitting an old
  token, pending persistence racing an active refresh, cancellation before the
  durable transition, access-token revocation reporting false uncertainty, and
  loss of a successful remote result after local cleanup failure. Additional
  RED cases required persisted restart evidence for a remotely uncertain grant,
  atomic marker transitions, and OAuth preservation of an overlapping
  refresh's uncertainty. Commit `bf01688` added failing contracts for the
  irreversible disconnect boundary and a known rotated grant. Commit `92c7c1a`
  then captured cancellation/timeout state recovery, provider-token versus
  local-CAS-token separation, fail-closed remote uncertainty, the progress
  callback lifetime, and the GUI exception boundary before those fixes.
  Commit `e482d0c` added the remaining RED contract for restoring a progress
  dialog when Cancel races the irreversible phase transition.
- Resolution: Removing Strava is now an explicit, confirmable asynchronous
  operation with separate remote-disconnect and local-only choices. Remote
  removal sends the authenticated revocation request over verified HTTPS and
  accepts only HTTP 200. It blocks new API permits, advances the athlete grant
  epoch, drains or aborts active requests and refreshes, and then persists the
  fail-closed pending state before any remote mutation. This ordering prevents
  an older refresh publication from restoring durable active state after the
  pending commit. Cancellation or timeout before that commit now reconciles the
  in-memory canonical token and cache with the latest durable pair, leaving a
  safe active grant usable. After the commit, cancellation is intentionally
  ignored, the UI removes its Cancel control, and the operation completes.
  Placeholder build credentials fail before any state change.
  An ambiguous refresh publication immediately blocks new requests in the
  current process and remains pending across restart. The latest observed token
  is used only for the provider request, while the durable token remains the
  local compare-and-swap expectation. Credentials are cleared only after a
  provider-confirmed HTTP 200 response or an explicit local-only override, and
  a successful remote result is retained if local cleanup subsequently fails.
  Access- and refresh-token revocation are both treated as revoking the
  associated grant, as specified by Strava. Successful refresh, OAuth
  activation, and removal publish their related state with targeted settings
  synchronization. Pre-revocation failures preserve remote uncertainty. The UI
  reports that uncertainty and local cleanup failures without claiming remote
  success where none is known, uses a stable GUI-thread context for phase
  updates, and catches asynchronous service exceptions.
- Verification: The 364 focused coordinator, publication, OAuth, settings,
  account-removal, session, and migration cases pass normally and again under
  strict ASan/UBSan/LSan with leak detection. All 50 coordinator cases also
  pass under ThreadSanitizer with uninstrumented Qt modules excluded. The
  complete application build, AppImage packaging consistency check, and
  top-level test matrix pass.

### CLOUD-009: Strava removal conflated provider and local CAS tokens

- Status: FIXED
- Code: `src/Cloud/StravaTokenRefresh.cpp` and
  `src/Cloud/StravaAccountRemoval.cpp`
- Impact: After a provider rotation whose local publication failed, removal
  correctly selected the newest observed token for remote revocation but also
  used it as the local credential-removal compare-and-swap expectation. The
  durable store still held the older token, so provider revocation could
  succeed while local cleanup always conflicted and remained pending.
- Test-first evidence: Commit `92c7c1a` reproduced the mismatch by requiring
  the rotated token at the provider boundary and the durable token at the
  publisher boundary.
- Resolution: The coordinator now passes separate remote and durable tokens in
  one removal transaction. The provider gets the latest known grant token;
  local compare-and-swap uses the token that actually committed durably.
- Verification: All 16 account-removal cases pass normally and under strict
  ASan/UBSan/LSan, and the complete matrix passes.

### THREAD-012: Aborted Strava removal could retain a stale canonical token

- Status: FIXED
- Code: `src/Cloud/StravaTokenRefresh.cpp`
- Impact: A refresh completing while removal drained requests could publish a
  new durable pair but have its caller result rejected by `removalInFlight`.
  Cancellation or timeout before the pending commit then restored active state
  without updating the canonical token or stale cache. A later refresh could
  send the superseded token and fail the otherwise active account.
- Test-first evidence: Commit `92c7c1a` added deterministic cancellation and
  timeout rows that held an active request, completed a rotating refresh during
  removal, aborted removal, and observed the old token on retry.
- Resolution: Every pre-commit removal-abort path reconciles canonical state
  with the durable token, remembers that token, and discards a cache whose pair
  no longer matches durable storage.
- Verification: Both RED rows pass in the 50-case normal, sanitizer, and
  ThreadSanitizer coordinator runs.

### MEM-021: Strava phase callback could target a destroyed progress dialog

- Status: FIXED
- Code: `src/Gui/AthletePages.cpp`
- Impact: A worker checked a cross-thread `QPointer` and then separately passed
  its raw widget pointer to `QMetaObject::invokeMethod`. Destruction between
  those operations could make the invocation target dangling during settings
  page or application teardown.
- Test-first evidence: Commit `92c7c1a` forbade the raw `QPointer` invocation
  target and required an independently owned GUI context before the fix.
- Resolution: The worker retains a shared, GUI-affine `QObject` context for the
  queued phase update. The progress-dialog `QPointer` is dereferenced only
  inside the GUI-thread callback. A shared irreversible flag also prevents a
  late visible Cancel action from changing operation state, and the queued
  phase update restores a dialog hidden by a boundary-racing Cancel before
  removing its Cancel control.
- Verification: The UI source contract and complete application build pass.
  Runtime widget lifecycle coverage remains tracked by `TEST-001`.

### CLOUD-010: Pre-revocation failures suppressed remote uncertainty

- Status: FIXED
- Code: `src/Cloud/StravaTokenRefresh.cpp` and
  `src/Cloud/StravaAccountRemoval.cpp`
- Impact: Cancellation, drain timeout, pending-state persistence failure, and
  a throwing removal operation defaulted `remoteAuthorizationMayRemain` to
  false even though provider revocation had not been confirmed. The UI could
  omit the warning that Strava might still authorize GoldenCheetah.
- Test-first evidence: Commit `92c7c1a` added failing cancellation, timeout,
  pending-storage, and throwing-operation expectations.
- Resolution: Coordinator-generated removal failures now default to
  fail-closed remote uncertainty. Explicit provider success can still preserve
  a known false value across a subsequent local-cleanup failure.
- Verification: The coordinator and account-removal suites pass normally and
  under strict sanitizers.

### DUR-009: Partial Strava credential publication has no automatic recovery

- Status: OPEN
- Code: `src/Cloud/StravaTokenPublication.cpp`,
  `src/Cloud/StravaCredentialPublisher.cpp`, and `src/Cloud/Strava.cpp`
- Impact: Refresh-token rotation must persist the new refresh token before the
  corresponding access token. If the later access-token or timestamp write
  fails, the coordinator correctly leaves authorization pending, but
  production does not schedule recovery. The in-process coordinator still
  knows the observed pair and the publication primitive can retry, but a
  restart loses that evidence and requires reauthorization or disconnection.
- Test: Inject a one-time access-token or timestamp persistence failure,
  restart against the same settings and vault, and require deterministic
  recovery without admitting API requests until the complete pair is active.
- Fix direction: Persist a minimal secure recovery journal for the observed
  pair and retry publication during startup. Never mark the grant active until
  every credential and metadata write has committed and synchronized.

### DUR-010: Credential-scope mirror failure is not propagated

- Status: OPEN
- Code: `src/Core/Settings.cpp`
- Impact: `mirrorCredentialScope()` logs a failed system-settings sync but its
  callers still cache and return the scope identifier. A later legacy or
  pre-initialization path can then select a different scope after restart,
  while credential migration may already have written to the first scope.
- Test: Inject system scope-mirror write and sync failures for global and
  athlete mappings. Require an empty/failure result, no cached scope, no vault
  write, no plaintext removal, and deterministic recovery after restart.
- Fix direction: Return and propagate checked mirror durability. Do not expose
  or cache a newly selected scope, migrate credentials, or scrub a source until
  both its canonical target mapping and required compatibility mirror are
  durable.

### DUR-011: Failed vault deletion is not retried in the same process

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialSettings.h`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: `removeChecked()` records a negative cache entry when vault deletion
  fails. A subsequent `value()` sees the durable pending-removal marker but
  returns through that cache entry before retrying the backend, so recovery is
  delayed until restart or explicit cache clearing.
- Test: Fail a vault removal, recover the backend without reconstructing
  `CredentialSettings`, read the credential again, and require a second removal
  attempt, cleared pending marker, and no credential resurrection.
- Fix direction: Pending removal must take precedence over negative cache
  entries. Cache only a durable deletion result, or invalidate the entry before
  each retry while preserving memory-only replacement semantics.
- Resolution: Reads inspect the exact durable pending-removal marker before
  any cache entry. A failed removal leaves that marker intact, advances no
  visible success state, and retries plaintext cleanup plus vault deletion on
  the next read in the same process or after restart. Successful deletion
  clears the marker only after the backend result is durable, while cache
  revisions invalidate stale positive, negative, and memory-only entries
  across instances and cooperating processes.
- Verification: The same-process and restart deletion regressions, revision
  failure cases for write/migration/removal, subprocess cache transitions, and
  the complete 150-case credential suite pass in normal, TSAN, and strict
  ASan/UBSan/LSan builds. The complete matrix passes 2,862 of 2,864 cases with
  zero failures and the two `TEST-002` source-contract skips.

### THREAD-010: Strava grant coordination is process-local

- Status: OPEN
- Code: `src/Cloud/StravaTokenRefresh.cpp` and
  `src/Cloud/StravaCredentialPublisher.cpp`
- Impact: Static mutexes, registries, epochs, and request permits coordinate
  service clones only inside one GoldenCheetah process. Two processes using the
  same athlete profile can concurrently rotate, revoke, install, or publish a
  grant and defeat the otherwise serialized state machine.
- Test: Run independent subprocesses against one disposable athlete settings
  tree and fake provider. Force overlapping refresh, OAuth, and removal
  operations and require one durable ordering with no stale token publication.
- Fix direction: Add a per-athlete interprocess lease around remote grant
  mutation and credential publication, backed by a durable generation checked
  before and after each network transition.

### THREAD-011: Started Strava settings commits can block callers indefinitely

- Status: OPEN
- Code: `src/Cloud/StravaCredentialPublisher.cpp`
  (`runOnSettingsThread`)
- Impact: Cancellation and deadline handling can abandon an operation that has
  not started on the settings thread. Once the GUI thread begins a credential
  write, the caller waits for its definitive result even after the deadline.
  This avoids reporting timeout while a mutation can later commit silently,
  but an indefinitely blocked credential backend can hang a worker or
  application teardown.
- Test: Block a settings backend after the GUI-side operation starts, then
  cancel and expire the deadline. Require bounded owner teardown and a durable,
  explicit unknown or pending result that startup recovery can resolve.
- Fix direction: Use a bounded or cancellable storage backend, or move the
  durable operation to an owned worker with a recoverable transaction identity.
  Do not return timeout while an untracked mutation can still commit.

### BUILD-010: AppImage credential gate treats absence of one marker as proof

- Status: FIXED
- Code: `src/Cloud/StravaOAuthPolicy.cpp`, `src/Core/main.cpp`,
  `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/MakeAppImageQt6.sh`,
  `appveyor/linux/after_build.sh`, and
  `.devcontainer/package-appimage.sh`
- Impact: Packaging claims Strava OAuth is configured whenever one literal
  placeholder is absent. A non-GoldenCheetah executable, missing or invalid
  client ID, empty or generic placeholder secret, stripped Strava code, and
  Type 1 AppImage can therefore pass the production gate.
- Test-first evidence: Commit `87da8ff` replaced marker fixtures with
  executable status probes and added failing contracts for the absent C++
  report, `/bin/true`, malformed or failed reports, missing Strava support,
  executable non-ELF input, and Type 1 and Type 2 images passed to the raw
  gate. Commit `dc75820` then required a separate completed-AppImage gate and
  post-package checks in every packaging path before their implementation.
  Follow-up RED rows captured the documented `your_client_secret` placeholder,
  byte-exact final-newline validation, `AppRun` versus an unrelated side
  binary, and signal cleanup before those refinements.
- Resolution: GoldenCheetah now exposes an exact, versioned
  `--goldencheetah-build-status` report before GUI or settings initialization.
  Its Strava value is produced by the same
  `StravaOAuthPolicy::hasUsableCredentials()` policy as runtime OAuth and
  reveals no credential value, fragment, length, or digest. The raw gate
  requires a regular executable ELF, rejects both AppImage magic versions,
  executes the status command with a deadline and isolated profile, bounds its
  output in a private temporary file, and accepts only the exact application,
  Strava-support, and configured protocol. The completed-image gate accepts
  only Type 2, extracts the complete payload into a private temporary
  directory, resolves `AppRun`, rejects an entrypoint escaping the payload,
  and applies the same raw gate to the actual entrypoint. Local, AppVeyor, and
  development-container packagers now check both the source executable and
  final AppImage.
- Verification: The packaging fixture matrix passes, including unrelated and
  malformed ELF files, executable non-ELF input, oversized output, failed
  status execution, missing Strava support, unavailable credentials, both raw
  AppImage types, and extracted configured/unavailable images. All 65 OAuth
  policy and source-wiring cases pass normally and under strict
  ASan/UBSan/LSan. A fully linked configured GoldenCheetah passes the raw gate,
  rejects extra status arguments without entering the GUI, and the complete
  top-level test matrix passes.

### BUILD-011: AppImage metadata is not bound to the binary source revision

- Status: OPEN
- Code: `src/Resources/linux/AppImagePackagingSupport.sh:91`,
  `src/Resources/linux/MakeAppImageQt6.sh:110`, and release orchestration
- Impact: `GC_SOURCE_REVISION` is checked only for hash syntax. Packaging does
  not prove that the commit exists, the worktree was clean, or the supplied
  binary was built from that tree. A plausible but incorrect revision can be
  written beside an unrelated image.
- Evidence: The local release workflow preserves a mode-0600 sidecar and hashes
  the transferred AppImage, but the repository packager does not bind the
  revision to a raw-binary hash or place a verifiable manifest in the image.
  AppVeyor does not generate the same sidecar.
- Test: Package a binary from revision A while claiming revision B and require
  rejection. Verify a manifest containing source revision, raw ELF hash,
  AppImage hash, toolchain identity, and boolean OAuth status across atomic
  `latest`/`previous` rotation.
- Fix direction: Build and package from a clean, identified source export;
  generate one canonical manifest before deployment, embed the non-recursive
  fields in the image, and verify source, binary, image, and sidecar hashes at
  promotion time.

### BUILD-012: Primary AppImage packaging omits the libsecret runtime

- Status: FIXED
- Code: `src/Resources/linux/MakeAppImageQt6.sh`,
  `src/Resources/linux/AppImagePackagingSupport.sh`, and
  `.devcontainer/package-appimage.sh`,
  `appveyor/linux/after_build.sh`, `appveyor/linux/install.sh`,
  `src/Core/CredentialStoreQtKeychain.cpp`, `src/Core/main.cpp`, and
  `contrib/qtkeychain/qtkeychain/libsecret.cpp`
- Impact: On a Linux desktop without a host `libsecret-1.so.0`, QtKeychain
  cannot open the native credential vault. GoldenCheetah fails closed instead
  of writing new plaintext credentials, but OAuth setup, token rotation, and
  legacy-credential migration cannot complete durably.
- Evidence: QtKeychain loads `secret-1` through `QLibrary`, so
  `linuxdeployqt` cannot discover it from the ELF dependency graph. The
  development-container packager compensates by copying the resolved library
  and license, but the primary Qt6 packager does not call that logic. A fresh
  primary-package artifact contained the offscreen plugin and embedded Python
  but no `libsecret-1.so.0`; it worked locally only because the host provides
  that library.
- Test-first evidence: The packaging fixture now requires a shared libsecret
  installer, AppDir and completed-AppImage status checks, runtime presence and
  exact license copies, fail-closed missing-license behavior, and use of the
  shared pre- and post-package gates by every AppImage path. The RED run fails
  at the first absent helper, `install_linux_keychain_runtime`. Independent
  review then added RED contracts for the eight QtKeychain symbols,
  AppDir-contained files, a complete LGPL-2.1 license, a temporary `DT_NEEDED`
  deployment probe, compiled and runtime-available libsecret status from the
  actual entrypoint, and AppVeyor build dependencies. The shell fixture fails
  at the absent entrypoint gate, while the CredentialSettings test fails to
  compile at the absent pure status-report function. A packaged runtime then
  returned
  `libsecret_runtime=unavailable` when both host libsecret paths were hidden,
  exposing that QLibrary did not use the executable's RUNPATH for this
  `dlopen`. Follow-up RED tests require the runtime in the executable's
  `lib/` directory, a contained regular-file resolver, and explicit
  QtKeychain use of that validated path. Final review added RED contracts for
  payload-origin resolution of the GLib/GIO/GObject/libgcrypt chain, defined
  `GLOBAL FUNC` symbols rather than `UND` names, reviewed QtKeychain and
  LGPL-2.1 digests, and meaningful libsecret copyright content. The new
  fixture first fails because the reviewed license digest constants are
  absent. A second independent final review found that libgpg-error still
  resolved from the host, shell AppRun wrappers were rejected, copyright
  notices were not required, linked installer destinations could be written
  before rejection, and deploy-probe cleanup needed an explicit regression
  contract. That cleanup contract passes on the current Bash implementation;
  the first new RED failure is the absent bundled libgpg-error runtime. The
  expanded fixture then covers unexpected host and unresolved dependencies,
  linked library and license directories, shell entrypoints, copyright
  notices, and hostile loader environment overrides. Final follow-up review
  found that absolute `DT_NEEDED` paths and linked extra dependencies still
  bypassed the generic parser, AppImage runtime variables were not emulated for
  shell wrappers, and extraction itself inherited loader overrides. Additional
  RED contracts cover those paths before promotion.
- Resolution: Every AppImage path now uses one shared installer and
  completed-image gate. A temporary `DT_NEEDED` probe makes linuxdeployqt
  discover libsecret's dependency chain and is removed before image creation.
  The installer resolves the real library through pkg-config, installs it in
  the executable's `lib/` runtime directory with a `$ORIGIN` dependency path,
  and includes the distribution copyright, full LGPL-2.1 text, and QtKeychain
  license. The payload gate rejects links or files escaping the AppDir,
  validates the ELF SONAME and all eight defined `GLOBAL FUNC` symbols
  resolved by QtKeychain, and requires GLib, GIO, GObject, libgcrypt, and
  libgpg-error to resolve from the payload. Every other dependency must either
  resolve as a canonical regular payload file or belong to a narrow Linux ABI
  allowlist; unresolved, absolute outside, linked outside, unrecognized, and
  unexpected host libraries fail the release. The installer rejects linked
  destination components before writing, adds `$ORIGIN` to libgcrypt, and
  checks reviewed license digests plus copyright markers and notices. The
  completed-image gate resolves the real AppRun entrypoint, accepts ELF and
  shebang wrappers, supplies `APPDIR`, `APPIMAGE`, and `OWD`, clears loader and
  stale AppImage overrides before extraction, and executes an exact bounded
  status protocol. GoldenCheetah reports compile-time `HAVE_LIBSECRET`
  separately from the specific libsecret backend's runtime availability. It
  validates the bundled regular file and passes its canonical absolute path to
  the vendored QtKeychain loader, so the package does not depend on host
  QLibrary fallback. AppVeyor now installs the required development and
  pkg-config packages.
- Verification: The packaging fixture matrix passes all installer, payload,
  symbol, license, symlink, AppRun, compile-support, runtime-availability, and
  packaging-path contracts. CredentialSettings passes 77 cases normally and
  under ASan/UBSan/LSan, the full application links, and the complete
  top-level test matrix passes 2,697 cases across 79 test programs with no
  failures or skips. The old production image is rejected because libsecret is
  absent. A real new primary AppImage passes both release gates; contains no
  deployment probe; includes all three license files; has no unresolved
  libsecret dependencies; and resolves GLib, GIO, GObject, libgcrypt, and
  libgpg-error from its own `lib/` directory. Most importantly, its real AppRun
  still reports `libsecret_runtime=available` when both host libsecret paths,
  the host libgpg-error SONAME, and its versioned target are replaced by empty
  bind mounts, reproducing the earlier RED setup without host fallback. The
  final boundary fixture, primary package gates, and complete 2,697-case matrix
  all pass after canonical dependency parsing and AppImage environment
  emulation.

### SEC-013: A desktop AppImage cannot keep its Strava client secret private

- Status: OPEN
- Code: `src/Core/Secrets.h`, `src/gcconfig.pri`, generated Makefiles, and
  configured GoldenCheetah executables
- Impact: A configured native client embeds its reusable application secret in
  the executable. Anyone receiving that binary can recover the value and
  impersonate the application, potentially consuming rate limits or causing
  provider sanctions that affect every user of that client identity.
- Evidence: The configured value can be matched between the private qmake
  configuration and the extracted AppImage ELF without source access. Strava's
  OAuth documentation calls the client secret private, but its current desktop
  flow still requires it for code exchange and does not document PKCE.
  Verbose qmake builds can additionally place it in command lines, logs, and
  generated Makefiles; remote build directories have been restricted to mode
  0700 as an operational mitigation.
- Test: Scan release payloads and build artifacts for sentinel credentials,
  verify restrictive permissions on unavoidable intermediates, and exercise a
  public-client or brokered flow without a reusable secret in the binary.
- Fix direction: Prefer a provider-supported PKCE/public-client flow. Until one
  exists, choose explicitly between per-user registered applications and a
  narrowly scoped server-side exchange service; neither obfuscating the binary
  nor keeping only the source define private solves distribution exposure.

## Low

### TEST-001: Strava disconnect UI lifetime coverage is source-only

- Status: OPEN
- Code: `src/Gui/AthletePages.cpp` and
  `unittests/Cloud/stravaOAuthPolicy/testStravaOAuthPolicy.cpp`
- Impact: The implementation guards the page and progress dialog lifetimes,
  owns the watcher beneath the page, binds completion to a context object, and
  shares cancellation state. However, the current UI regression only inspects
  source wiring. A future refactor could reintroduce a callback after page
  destruction or leave controls disabled without failing a runtime test.
- Test: Instantiate the credentials page with an injectable fake disconnect
  operation. Cover cancellation, page destruction while pending, provider
  failure, local-cleanup uncertainty, and success; verify callback lifetime,
  button state, and resulting settings in every path.
- Fix direction: Inject the disconnect operation behind the existing service
  contract and add a Qt widget lifecycle suite using a disposable athlete
  profile.

### TEST-002: Source-contract tests skip in out-of-source builds

- Status: OPEN
- Code: `unittests/Core/signalSafety/testPatternDetection.cpp`,
  `unittests/Core/signalSafety/testTreeSafety.cpp`, and their source-checking
  scripts
- Impact: A clean out-of-source `make check` silently skips both unsafe
  connection and unsafe tree-child source checks. CI can therefore report a
  green full matrix without enforcing these two contracts.
- Evidence: The clean DUR-005 verification matrix ran 81 QtTest programs and
  reported 2,817 passes, no failures, and two skips: `testUnsafeConnects()`
  could not find `check_unsafe_connects.py`, and `testUnsafeChildAccess()`
  could not find `check_unsafe_tree_child.py`.
- Test: Run the two suites from a clean build directory and require both source
  checks to execute without `QSKIP`.
- Fix direction: Resolve the scripts through `QFINDTESTDATA`, a compile-time
  source-root path, or copy them into the test runtime directory as part of the
  build.

### BUILD-008: Qt 6.8.3 reports impossible QVariant inline-storage overflows

- Status: OPEN
- Code: `src/Charts/GoldenCheetah.cpp:969`,
  `src/Charts/GoldenCheetah.cpp:1166`,
  `src/Gui/Perspective.cpp:1642`, and the Qt 6.8.3 build image
- Impact: GCC emits repeated `-Warray-bounds` diagnostics while moving the
  large `LTMSettings` metatype from a temporary `QVariant`. The warnings mask
  real bounds diagnostics such as MEM-018, but this specific path is not a
  GoldenCheetah memory error: Qt heap-allocates types larger than its inline
  variant storage.
- Evidence: A minimal large-`QString` metatype reproduces the warning only
  through Qt 6.8.3's rvalue `QVariant::value()`. The lvalue form and strict
  ASan/UBSan/LSan execution are clean. Qt fixed the impossible generated branch
  in QTBUG-140064 (qtbase commit `46feeec`).
- Test: Compile the minimal large-metatype reproducer at
  `-O2 -Werror=array-bounds` and exercise chart property serialization under
  ASan/UBSan.
- Fix direction: Upgrade or backport Qt's header-only fix. A local
  `const QVariant` intermediate is an acceptable temporary workaround; do not
  suppress `-Warray-bounds` globally.

### BUILD-009: Release builds have no project-warning gate

- Status: OPEN
- Code: release qmake configuration and CI
- Impact: The clean MEM-019 release build succeeds with 160 warnings from
  project, generated, vendored, and toolchain code. Genuine findings
  (BLE-006 and GUI-003) appear beside false positives and intentional
  fallthroughs, making regressions easy to miss.
- Evidence: Removing MEM-018 reduced the clean count from 162 to 161, and
  fixing MEM-019 reduced it to 160, without any automated budget check.
  Remaining project warnings include implicit fallthrough in
  `GenericLegend.cpp` and `RideMetadata.cpp`, Qt-macro control flow in
  `Perspective.cpp`, declaration style in `Route.cpp`, and an enum conversion
  in `FitRideFile.cpp`.
- Test: Classify the current warning set by ownership and compiler version,
  then fail CI on any new project warning and progressively lower the baseline.
- Fix direction: Make project-owned targets warning-clean with explicit control
  flow and conversions, backport BUILD-008, and quarantine unavoidable
  generated/vendor diagnostics with narrow source- and compiler-specific
  suppressions.

### BUILD-002: AppImage omits the Qt offscreen platform plugin

- Status: FIXED
- Code: `src/Resources/linux/MakeAppImageQt6.sh`
- Impact: The Qt build provided `libqoffscreen.so`, but `linuxdeployqt` bundled
  only `libqxcb.so`. The packaged GUI therefore aborted during display-free
  release and CI smoke tests, forcing every AppImage check to depend on a live
  X11 session and repeatedly obscuring real startup regressions with the same
  packaging error.
- Test-first evidence: A direct launch with a clean profile,
  `QT_QPA_PLATFORM=offscreen`, and no display exited 134. Qt reported that the
  offscreen platform plugin could not be found and listed xcb as the only
  available backend. Inspection confirmed that the build image contained the
  plugin while the generated AppDir did not.
- Resolution: Packaging now resolves Qt's plugin directory through `qmake`,
  fails explicitly if `libqoffscreen.so` is unavailable, and copies it after
  `linuxdeployqt` deployment. The script also runs the finished AppImage for ten
  seconds with a disposable HOME and offscreen software rendering; any result
  other than the expected timeout status rejects the package.
- Verification: The script passes `bash -n`. The rebuilt AppDir contains both
  `libqxcb.so` and `libqoffscreen.so`, preserving normal desktop startup while
  enabling headless execution. The 166,259,192-byte AppImage with SHA-256
  `9dd770ee212fcd8e3f5adf2547602dd926c9061b7aa4885e7a72e041d11347c3`
  remained stable for separate 15-second direct X11 and display-free offscreen
  launches; both clean-profile logs contained only translator debug notices.

### BUILD-013: Qt 6.8 MOC sees an incomplete RideItem property type

- Status: FIXED
- Code: `src/Metrics/RideMetadata.h`
- Impact: `RideMetadata` exposes `RideItem*` through `Q_PROPERTY` and stores it
  in `QPointer`, but the header provides only the project's forward declaration
  to MOC. Qt 6.8 generates metatype code that requires the complete type, so a
  full application build can fail in generated `moc_RideMetadata.cpp`.
- Test-first evidence: The full Qt 6.8 build failed while compiling the
  generated metadata translation unit because `RideItem` was incomplete.
  Focused Core-only builds did not compile that generated application target
  and therefore did not expose the defect.
- Resolution: `Q_MOC_INCLUDE("RideItem.h")` gives MOC the complete property
  type without introducing a normal C++ include dependency into the header.
- Verification: The complete Qt 6.8 application build succeeds and all 79
  QtTest programs pass with zero failures.

### DEV-007: ANT FE-C spindown result aliases the zero offset

- Status: OPEN
- Code: `src/ANT/ANT.h`, `src/ANT/ANTlocalController.cpp`,
  `src/Train/TrainSidebar.cpp`
- Impact: Successful ANT+ FE-C spindown calibration displays the zero-offset
  field as the spindown duration. This can report 0 ms or duplicate the offset,
  preventing users from verifying the trainer's result. Calibration execution
  and activity recording are unaffected.
- Regression test: On an idle production `ANT`, assign distinct zero-offset and
  spindown values and require each getter to return its corresponding field.
- Fix direction: Return `calibration.getSpindownTime()` from
  `ANT::getCalibrationSpindownTime()`. Keep synchronization and coherent
  publication of calibration state under `DEV-003`.

### THREAD-005: Cloud SSL callbacks read a GUI parent in worker threads

- Status: OPEN
- Code: `src/Cloud/Strava.cpp`, `src/Cloud/Nolio.cpp`, and other provider
  `onSslErrors` implementations; `src/Cloud/CloudService.cpp`
- Impact: The base SSL helper now creates warnings on the GUI thread, but each
  provider first evaluates `context->mainWindow` in its own thread. Concurrent
  context or window teardown can race that raw GUI pointer read.
- Test: Deliver SSL errors while closing the athlete and main window under TSAN,
  requiring no worker-thread GUI access and no warning after owner destruction.
- Fix direction: Pass only value data or a guarded Context identity from the
  provider, then resolve the parent entirely on the GUI thread.

### THREAD-006: Nested start listeners can reorder cloud lifecycle signals

- Status: OPEN
- Code: `src/Cloud/CloudService.cpp` (`CloudServiceAutoDownload::startDownload`)
- Impact: The worker starts before `autoDownloadStart` finishes notifying all
  direct listeners. If an early listener runs a nested event loop, a fast worker
  can deliver `autoDownloadEnd` before later listeners receive the original
  start signal, leaving observers in the wrong state.
- Test: Attach multiple start listeners, run a nested loop in the first, and
  complete the provider inline; every observer must see start before progress
  or end.
- Fix direction: Publish start before launching the worker, or hold completion
  events until start notification has fully unwound.

### CLOUD-008: Strava API host migration is still duplicated in call sites

- Status: OPEN
- Code: `src/Cloud/Strava.cpp` and
  `src/Train/StravaRoutesDownload.cpp`
- Impact: Strava announced that its API base URL is changing from
  `https://www.strava.com/api/v3` to `https://api-v3.strava.com`, with the new
  host available from 4 January 2027. The current host is repeated throughout
  activity, stream, upload, athlete, route, and GPX paths, making a deadline
  migration easy to miss or apply inconsistently.
- Test: Enumerate every Strava API request through one endpoint policy and
  verify allowed HTTPS hosts, paths, redirects, and a provider-controlled
  migration switch without changing OAuth hosts.
- Fix direction: Centralize API URL construction now, retain the documented
  current host until Strava opens the replacement, then validate and switch in
  one place before the provider's removal deadline is announced.

## Verification Baseline

The complete containerized release matrix after SEC-021 passes:

- 81 QtTest suites
- 3,126 passed
- 0 failed or blacklisted
- 7 expected platform-only skips on Linux
- Qt 6.8.3 on Ubuntu 24.04

The registered matrix includes the AppImage packaging consistency test and the
32-case Strava OAuth policy suite. Production AppImages are packaged from
committed source only after this matrix, the predecessor remains available as
the rollback image, and the local sidecar records the packaged commit and
SHA-256 without making repository documentation depend on local artifact
state.

PARSE-005's 126 focused tests and the related 11 RideFile ownership tests also
pass under strict ASan/UBSan/LSan with leak detection. Earlier fixed
memory/thread findings retain the focused sanitizer and TSAN evidence recorded
in their entries.

This baseline is not evidence for any remaining OPEN finding. Each open item
still requires its listed RED regression before implementation. No whole-suite
fuzzer or production-scale profiler campaign has been completed.
