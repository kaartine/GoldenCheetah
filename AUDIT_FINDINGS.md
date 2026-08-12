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

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialSettings.h`,
  `src/Core/CredentialStoreQtKeychain.cpp`,
  `src/Core/CredentialStoreQtKeychain.h`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: A timed-out QtKeychain job is switched to auto-delete and released
  while its backend operation continues. The caller receives `Unavailable` and
  releases the per-credential operation lock, but a timed-out write or removal
  can still complete later and reorder against a retry, replacement, or delete.
- Test-first evidence: The initial delayed-job regression passed only two of
  eight cases. Three timed-out mutation cases incorrectly returned a definite
  result, never-finishing write and remove subprocesses exceeded their
  watchdogs, and a second write started while the first backend lease was
  active. A direct ThreadSanitizer run then found a separate race between the
  GUI-thread result publication and the waiting worker's stack read.
- Resolution: QtKeychain operations now retain a process-wide owner token until
  the backend emits its terminal signal. Mutations additionally hold a
  cross-process `QLockFile` and an owner-only, atomically published and
  directory-synced pending marker before the backend starts. A mutation timeout
  returns the new `Indeterminate` status, preserves both leases, invalidates the
  in-memory secret cache, and blocks reads, retries, and cleanup state changes
  until terminal acknowledgement. A stale or destroyed job cannot release a
  newer owner, and marker creation, validation, or removal failure remains
  quarantined. The worker handoff now publishes its result under an explicit
  mutex before the blocking invocation returns.
- Verification: The final credential program passes 410 cases with zero
  failures and seven Linux platform skips. The final 52-case timeout,
  serialization, marker, process-crash, and cache matrix passes both strict
  ASan/UBSan/LSan and ThreadSanitizer runs with no reports or races. Final
  production and test sources pass MinGW64 C++17 syntax checks. The complete
  out-of-source matrix runs 81 QtTest programs: 3,154 cases pass, none fail or
  blacklist, and seven platform cases skip. One earlier normal full run hit an
  unrelated enrollment snapshot comparison; the affected global and athlete
  cases then passed 20 isolated repetitions and a clean full rerun without
  semantic divergence.
- Residual: A process crash during an in-flight mutation intentionally leaves
  the durable marker fail-closed; automatic reconciliation of that
  indeterminate backend outcome is not implemented. Native Windows Credential
  Manager calls may block synchronously on the GUI thread, and worker dispatch
  still uses a blocking queued invocation. This fix guarantees ordering and
  explicit uncertainty, not bounded completion of a wedged native backend; the
  broader availability problem remains tracked by `THREAD-011`.

### THREAD-015: OpenData export traverses live athlete state from its worker

- Status: FIXED
- Code: `src/Cloud/OpenData.cpp`, `src/Cloud/OpenData.h`,
  `src/Cloud/OpenDataCaptureStateMachine.*`,
  `src/Cloud/OpenDataCaptureUtils.*`, `src/Cloud/OpenDataExport.*`,
  `src/Cloud/OpenDataSummaryStatistics.*`,
  `src/Cloud/OpenDataTransport.*`,
  `src/Cloud/OpenDataTemporaryArchive.*`,
  `src/Cloud/OpenDataUploadWorker.*`, `src/Core/RideCache.h`, and
  `src/Core/RideDB.y`
- Impact: `OpenData::run()` retains a raw `Context *` and calls
  `RideCache::save(true)`, traverses `RideCache::rides()`, dereferences
  `RideItem` paths, opens ride files, and updates athlete settings from its
  `QThread`. Concurrent ride edits, cache destruction, athlete closure, or
  application shutdown can therefore race the export, produce an inconsistent
  archive, or dereference destroyed objects. The heap-allocated thread has no
  owner or `deleteLater()` connection, so its QObject lifetime is also
  unbounded after completion.
- Test-first evidence: The new request, lifecycle, capture-state, source
  identity, short-I/O, archive, cancellation, late-cancellation, receiver
  deletion, and owner-thread regressions initially failed to compile against
  the monolithic worker or exposed the missing behavior. A separate ordering
  regression demonstrated that the summary could be captured before the source
  manifest, permitting a stale summary to be paired with newer activity files.
  Final review then reproduced the inverse race: startup refresh settlement
  could mutate metrics after the manifest. Same-inode, same-size content
  replacement with a restored mtime also passed the metadata-only identity
  check, and a renamed Unix workspace survived lease cleanup.
- Resolution: Capture is now an owner-thread state machine. It waits for startup
  loading and all pending refresh generations to settle, records every selected
  source path and SHA-256 identity before writing the summary, copies and hashes
  the exact raw source bytes through short-I/O loops, and revalidates identities
  before publication. Compressed inputs are verified before decompression.
  Unix leases retain a directory descriptor and remove renamed workspaces by
  inode. The worker receives only an immutable request and archive path, owns
  its native thread, supports cooperative cancellation and joining, linearizes
  terminal success against late cancellation, and never retains athlete graph
  pointers. Archive construction, validation, transport, summary aggregation,
  temporary storage, and lifecycle policy are separated into focused modules.
- Verification: The final OpenData export program passes 30 cases normally and
  30 under strict ASan/UBSan/LSan, with five Windows-only skips and no sanitizer
  report. Its 13-case worker-state matrix passes ThreadSanitizer without
  suppressions. The 17-case capture utility matrix also passes strict
  ASan/UBSan/LSan. The Qt 6.8.3 application builds, and the complete out-of-source
  matrix passes 89 programs and 3,331 cases with zero failures or blacklisting
  and 12 expected platform skips.
- Residual: The two native Qt network-event-loop tests pass normally and under
  ASan, but an uninstrumented Qt 6.8.3 library produces a ThreadSanitizer
  file-descriptor report between `QCoreApplication::postEvent` and
  `QEventDispatcherUNIX`; the worker-state TSAN run therefore excludes those
  two tests rather than suppressing the report. Owner-thread refresh settlement,
  source hashing, parsing, CSV generation, summary generation, and validation
  can still pause the UI and observe cancellation only between state-machine
  advances; this is tracked by `PERF-009`.

### SEC-024: OpenData temporary archives lack verified Windows isolation

- Status: FIXED
- Code: `src/Cloud/OpenDataTemporaryArchive.cpp`,
  `src/Cloud/OpenDataTemporaryArchive.h`, and
  `unittests/Cloud/openDataExport/testOpenDataExport.cpp`
- Impact: Windows workspace creation supplied a private ACL but never verified
  that the filesystem persisted it. The lease retained only path strings, so
  the directory could be renamed or replaced between validation and file
  creation. A failed `CreateDirectoryW` path also removed the requested name
  unconditionally, which could delete a pre-existing empty directory created by
  a racing process.
- Test-first evidence: Windows-only regressions cover persistent owner-only
  protected ACLs, rejection after replacing a file DACL with a public one,
  active-directory rename denial, abandoned non-private workspace rejection,
  and preservation of a pre-existing directory after failed creation. The
  deterministic pre-existing-name regression first failed to link because no
  isolated creation seam existed.
- Resolution: A lease now retains a no-share-delete directory handle from
  successful creation through cleanup. Workspace and child handles reject
  reparse points, require exact case-preserving paths, verify persistent ACL
  support plus one protected owner-only ACE, and use handle-based delete
  disposition. Child files remain delete-pending until validation and ownership
  transfer complete. Post-create validation failures remove the exact empty
  directory through its retained handle when that handle resolves to the
  expected path. Abandoned cleanup requires the same private workspace ACL and
  never traverses directories or reparse points. Unix leases retain a directory
  descriptor, clean children through `unlinkat()`, and locate a renamed
  workspace by device and inode before removing it.
- Verification: The portable and Unix paths pass the 30-case normal and strict
  ASan/UBSan/LSan OpenData export suites. Five Windows-specific cases are
  registered and skipped on Linux. The production Windows link already includes
  `Advapi32`; native Windows runtime execution remains required before claiming
  platform coverage.
- Residual: Win32 `CreateDirectoryW` does not atomically return a directory
  handle. A same-user process that guesses the random UUID could replace the
  directory in the narrow interval before `CreateFileW`; removing that
  same-privilege race would require native NT object-manager APIs. If the
  post-create handle cannot be opened or resolves to a different path, cleanup
  still refuses path-based deletion of a possible replacement.

### PARSE-007: CPX readers trust attacker-sized and truncated cache layouts

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`,
  `src/FileIO/RideFileCache.h`,
  `src/FileIO/RideFileCacheIntegrity.cpp`, and
  `src/FileIO/RideFileCacheIntegrity.h`
- Impact: CPX headers supplied unchecked unsigned block counts to allocation,
  offset, seek, and raw-read operations. Truncated headers and payloads,
  arithmetic overflow, growth during a read, or a short scalar read could
  consume excessive memory, read an unintended block, or publish a partially
  overwritten value. Multi-value consumers could combine valid values and
  synthetic zeroes from one changing cache into a result row.
- Test-first evidence: Malformed-header, count-abuse, exact-size, short-read,
  growth, invalid-index, scalar failure-atomicity, sticky-failure, single-pass
  validation, and batch-row regressions failed against the original raw
  `QDataStream` paths or first exposed the missing failure state.
- Resolution: `RideFileCacheIntegrity` now inspects the complete native CPX
  layout under checked 64-bit arithmetic, enforces a 256 MiB ceiling, requires
  exact file size and exact reads, and verifies size again after each read.
  `PartialReader` validates once, publishes scalars only after a complete read,
  becomes permanently invalid after seek, truncation, or mutation failure, and
  causes batch consumers to discard the entire affected row. All constructor,
  block, zone, best, TIZ, and aggregate read paths fail closed through the same
  layout model.
- Verification: The focused integrity program passes 31 normal cases and the
  refresh/integration program passes seven. Strict sanitizer, ThreadSanitizer,
  application, and complete-matrix results are recorded in the final
  verification baseline below.
- Residual: CPX remains a native-endian, native-layout local cache format rather
  than a portable interchange format. Invalid or obsolete caches are ignored
  and recomputed from the source activity; they are never treated as
  authoritative workout data.

### PARSE-008: Activity CRC trusts a mutable file size and buffers the whole file

- Status: FIXED
- Code: `src/FileIO/RideFileCRC.cpp`, `src/FileIO/RideFileCRC.h`,
  `src/FileIO/RideFile.cpp`, `src/FileIO/RideFile.h`,
  `src/FileIO/RideFileCache.cpp`, and `src/Core/RideItem.cpp`
- Impact: `RideFile::computeFileCRC()` allocates the file's complete reported
  size, ignores the raw-read result, and checksums that full allocation. A very
  large or sparse activity can exhaust memory. If the file shrinks or a read
  fails after sizing, the checksum includes an uninitialized tail and becomes
  nondeterministic.
- Test-first evidence: The new CRC suite initially had no bounded input seam or
  native snapshot hooks. Its RED cases then exposed premature EOF, failed and
  size-changing reads, a missing pre-read size limit, atomic path replacement,
  overwrite with restored mtime, and incomplete Windows identity policy. A
  cache refresh with a missing source also invoked its writer before the caller
  was made fail-closed. Finally, a MinGW fixture that lets the first Qt header
  select a Vista target failed on `FILE_ID_INFO` before the Windows include
  order was corrected.
- Resolution: CRC-16/ISO-3309 is now computed in 64 KiB chunks with exact
  short-read, trailing-byte, error, and position-restoration checks. Sources
  larger than 512 MiB are rejected before reading. File CRC publication
  requires matching native handle and path snapshots before and after the
  stream: device/inode/size/nanosecond mtime and ctime on POSIX, and stable
  file identity/size/write/change time on Windows. Windows 8+ uses
  `FILE_ID_INFO`; explicitly older targets safely use the legacy volume and
  file-index identity. Active callers use a checked result API and fail closed;
  the deprecated value-only adapter remains for source compatibility. Cache
  refresh still computes valid in-memory arrays when source CRC capture fails
  but skips persistence, and `RideItem` preserves its prior CRC on failure.
- Verification: The 32-case CRC suite passes normally, under strict
  ASan/UBSan/LSan with leak detection, and under ThreadSanitizer without
  suppressions. The related 13-case ownership and nine-case cache-refresh
  suites retain normal and sanitizer coverage. The reusable MinGW syntax test
  passes both an include-order fixture and an explicit Windows Vista target.
  The production application build and the complete matrix below pass from the
  final source state.
- Residual: A 16-bit checksum has unavoidable collisions, and a source can
  still change after final validation; the CPX remains a disposable local
  cache rather than authoritative activity data. Separate cache invariants are
  tracked as `DATA-012`, `DATA-011`, and `PERF-010`.

### DATA-010: Planned and completed activities can alias one CPX cache

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`,
  `src/FileIO/RideFileCacheIntegrity.cpp`, `src/Core/RideItem.cpp`,
  `src/Core/DataFilter.cpp`, `src/R/RTool.cpp`, and
  `src/Python/SIP/Bindings.cpp`
- Impact: Most callers derived a cache filename from the activity basename
  under the completed-activity cache directory. A planned and a completed
  activity with the same filename could therefore overwrite or read each
  other's mean-max and time-in-zone values. `RideItem` stale checks and refresh
  paths also rebuilt source paths from the completed directory even when the
  item belonged elsewhere.
- Test-first evidence: Path regressions first demonstrated the completed and
  planned collision, rejected traversal and outside-root sources, and covered
  canonical matching through a symlinked activity root. A final fixture writes
  two real CPX files for identical planned/completed filenames and requires
  both `best` and `tiz` reads to return their distinct values.
- Resolution: Cache paths are now derived from a validated canonical source
  path. Completed activities retain the historical cache namespace while
  planned activities use `cache/planned`. `RideItem` stale, refresh, and
  file-cache paths preserve the item's actual directory. Item-aware `best` and
  `tiz` overloads are used by ranking, DataFilter, R, and Python callers.
  Temporary or purely in-memory rides compute without persistence when no safe
  completed/planned cache target exists.
- Verification: The seven-case refresh/integration program covers fixed-zone
  refresh, repeated refresh, memory-only calculation, namespace separation,
  actual best/TIZ values, and atomic batch failure. The complete build and
  matrix results are recorded below.
- Residual: The legacy filename-only `best` and `tiz` overloads remain
  completed-activity APIs for compatibility. New code with a `RideItem` must
  use the item-aware overload.

### DATA-012: CPX CRC is not bound to the in-memory RideFile

- Status: FIXED
- Code: `src/FileIO/RideFileCRC.cpp`, `src/FileIO/RideFile.cpp`,
  `src/FileIO/RideFileCache.cpp`,
  `src/FileIO/RideFileCacheIntegrity.cpp`, `src/Core/RideItem.cpp`, and
  the built-in `RideFileReader` declarations
- Impact: CPX persistence hashes the activity pathname but computes its payload
  from an independently held `RideFile`. A dirty or otherwise unprovenanced
  in-memory ride can therefore be persisted under the unchanged on-disk file's
  CRC. A later reload can accept derived arrays that do not describe the source
  activity named by the cache header.
- Test-first evidence: The original refresh path attempted persistence for a
  raw point mutation whose derived payload no longer matched the source.
  Additional RED cases exposed unprovenanced save handling, source replacement
  immediately before commit, blocking FIFO opens, a source rewrite after
  staging, unaudited readers receiving staged paths, and a zero-W-prime
  distribution writing a nonzero bin.
- Resolution: Stable regular files are copied into a private stage while
  computing a size-bound SHA-256 fingerprint and the legacy CRC16. Native file
  identity and timestamps are checked around both source reads; staged bytes
  are independently rehashed before and after parsing. Readers now fail closed
  to their original path unless explicitly audited as single-file readers, so
  CSV/Polar sidecar inputs and unknown readers remain unprovenanced.
  Parsed rides retain the strong source provenance only after postprocessing,
  invalidate it on modification, and rebind it through the same reader policy
  after save or revert. CPX refresh still computes usable in-memory results,
  but persistence additionally requires matching provenance, an independent
  fresh parse with byte-identical serialized payload, and a final source check
  after the temporary CPX is flushed and immediately before atomic commit.
- Verification: The 41-case fingerprint/staging suite, 35-case CPX integrity
  suite, and 22-case refresh/integration suite pass normally, under strict
  ASan/UBSan/LSan with leak detection, and under ThreadSanitizer without
  suppressions. The MinGW header-order syntax test, complete Qt 6.8.3 release
  build, and 92-program matrix also pass; the matrix reports 3,411 passed, zero
  failed or blacklisted, and 12 expected Linux skips.
- Residual: An external source can still change after the final validator and
  before `QSaveFile` renames the CPX. The installed cache is self-detecting and
  will be rejected after such a race, but the refresh can still report
  successful persistence that immediately requires rebuilding. `DATA-011`
  now binds every accepted cache read to a strong source fingerprint and
  authenticated CPX stream. Zone/configuration changes are not part of the
  same transaction and are tracked by `DATA-014`. CSV/Polar sidecar rides
  deliberately forgo persistent CPX, and verified refresh performs a second
  parse/compute. Arbitrary concurrent mutation of one `RideFile` remains an
  application-level data race even though unmatched payloads are no longer
  persisted.

### DATA-013: Planned CPX lifecycle operations use the completed namespace

- Status: FIXED
- Code: `src/Core/RideCacheRemoval.cpp`,
  `src/Core/RideCache.h`,
  `src/Core/RideCache.cpp`, and
  `src/FileIO/RideFileCacheIntegrity.cpp`
- Impact: Planned CPX files now live below `cache/planned`, but single and
  batch deletion and rename still address `cache/<base>.cpx`. Deleting or
  renaming a planned activity therefore leaves its real CPX behind and can
  delete or rename a completed activity's same-basename CPX instead.
- Test-first evidence: Against the unchanged implementation,
  `plannedRemovalDeletesOnlyPlannedCpx()` failed with six prior cases passing:
  the completed CPX was empty after deletion instead of retaining its distinct
  fixture contents. Companion regressions cover batch deletion and rename with
  planned and completed activities sharing one basename.
- Resolution: One source-aware helper constructs the planned or completed
  activity path and delegates CPX placement to
  `cachePathForActivity()`. Single and batch removal now share the same
  derived-file cleanup path, and rename addresses the old and new CPX only in
  the selected namespace. Notes and CPI retain their established root-cache
  placement.
- Verification: The focused removal program reports 16 passes: 14 substantive
  slots plus QtTest initialization and cleanup. It passes normally, under
  strict ASan/UBSan/LSan with leak detection, and under ThreadSanitizer without
  suppressions. The complete Qt 6.8.3 release build and successful 92-program
  offscreen matrix also pass; the matrix reports 3,442 passed, zero failed or
  blacklisted, and 12 expected Linux skips.
- Residual: Derived-file rename errors remain ignored after the activity file
  has moved and are tracked by `DATA-016`. Archive and cleanup failures remain
  non-transactional and are tracked by `DATA-018`.

### DATA-017: Filename-only removal can select the wrong activity namespace

- Status: FIXED
- Code: `src/Core/RideCacheRemoval.cpp`,
  `src/Core/RideCache.h`,
  `src/Charts/CalendarWindow.cpp`,
  `src/Gui/BatchProcessingDialog.cpp`,
  `src/Gui/PlanWizards.cpp`,
  `src/Gui/SplitActivityWizard.cpp`, and
  `src/Planning/PlanBundle.cpp`
- Impact: Startup retains planned and completed activities with the exact same
  filename and orders the completed item first, but removal searches only by
  filename. Removing the planned item can therefore archive the completed
  source, remove its completed CPX, and leave the requested planned activity
  untouched. Current-ride removal can make the same wrong selection.
- Test-first evidence: With the namespace fix but before the identity change,
  the focused program reported 11 passes and two failures.
  `currentRemovalUsesSelectedNamespace()` left the planned item instead of the
  completed item, and `ambiguousFilenameRemovalFailsClosed()` showed that the
  legacy filename-only call returned success instead of rejecting the
  collision.
- Resolution: Single, current, archived, calendar, plan, split, and explicit
  batch paths now carry `RideItem*` identity through removal. Batch processing
  uses its stored `(filename, planned)` identity. Lookup and model mutation
  compare the complete identity, while compatibility wrappers accept a
  filename only when it resolves to exactly one live item and otherwise fail
  closed. Batch removal snapshots its input before mutating the live ride list,
  so even an aliased `rides()` argument has defined iteration semantics.
- Verification: Exact-name collision tests cover selected-current removal,
  legacy single and batch rejection, explicit namespace lookup, and explicit
  batch identity. They are
  included in DATA-013's 16-pass normal, ASan/UBSan/LSan, and ThreadSanitizer
  runs, the complete release build, and the 92-program matrix.

### DATA-018: Activity deletion reports success after archival failure

- Status: FIXED
- Code: `src/Core/RideCacheRemoval.cpp`, `src/Core/RideCache.cpp`,
  `src/Core/RideCache.h`, `src/Core/RideCacheCallbackGuard.h`,
  `src/Core/RideItem.cpp`, `src/Core/RideItem.h`, `src/Core/Context.h`,
  `src/Gui/SaveDialogs.cpp`,
  `src/Gui/SaveDialogs.h`,
  `src/Charts/CalendarWindow.cpp`, `src/Gui/MainWindow.cpp`,
  `src/Gui/BatchProcessingDialog.cpp`, `src/Gui/BatchProcessingDialog.h`,
  `src/Gui/PlanWizards.cpp`, `src/Gui/PlanWizards.h`,
  `src/Gui/SplitActivityWizard.cpp`, `src/Gui/SplitActivityWizard.h`,
  `src/Gui/SplitActivityWorkflow.h`, `src/Planning/PlanBundle.cpp`, and the
  focused Core, FileIO, and Gui test projects
- Impact: Removal evicts the item from the model and deletes any previous
  backup before moving the source to the backup directory. If that move fails,
  the operation shows a dialog but continues deleting derived files and
  returns success. The live source may become invisible until restart, the
  previous backup is lost, and missing-source failures can leave no recoverable
  activity copy. Derived-file deletion errors are also ignored.
- Test-first evidence: Initial failpoint rows exposed source and derived-file
  mutation windows plus an ignored directory-sync failure. Expanded RED cases
  then covered unsafe names, planned/completed backup aliasing, linked-save
  failure, missing loaded ride data, locks, symlinks, every cleanup target, and
  batch stop semantics. A final rollback review produced 11 simultaneous RED
  failures: source restore and restore-sync failures replaced the only current
  backup with an older one, all three atomic moves mishandled partial effects,
  linked recovery left a half-unlinked pair, processor exceptions escaped, and
  the compatibility batch boolean changed meaning. Separate RED runs proved
  that a first-use planned-backup directory was not parent-synced and accepted
  a symlink outside the backup namespace. Independent final review added RED
  cases for a failed directory-sync retry, same-namespace incoming links,
  signal-time selection, linked-peer or target destruction during metadata and
  compensation callbacks, peer rename before rollback, and mutation or
  destruction of a later batch target. The selected normal run reported six
  failures, three linked rows crashed, and strict ASan found the batch
  use-after-free at the raw pending pointer. A final four-row RED regression
  then showed that destroying the cache or context from the first item's model
  removal signal made the batch coordinator continue through the deleted
  `RideCache` and crash in its final estimator refresh. Follow-up adversarial
  review found an ASan heap-buffer-overflow when a model-removal callback first
  removed an earlier sibling, because the original target index had become the
  new vector size. A real linked-save rename also returned
  `RecoveryRequired` after an otherwise successful deletion, while delete
  callers rejected a legitimate post-save activity identity. Separate RED
  save-dialog runs reported two passes and ten failures and reproduced
  heap-use-after-free in both Save Single and Save on Exit when their nested
  save callbacks destroyed the dialog. Transaction tests also showed that a
  durable save was reported as failed when finalization or `markClean` destroyed
  the saved ride. Repeat Plan could reach target removal with an empty copy set,
  and a shared plan reader could retain a raw, destroyed `Context*`. The final
  adversarial pass rejected unsafe linked-peer identities and incoming links to
  either member before any save, exposed invalid nested model-removal ranges and
  a deleted selection during model signals, and showed that staging cleanup was
  misclassified as a partial commit. It also reproduced a missing single-item
  refresh cancellation and ASan use-after-free in stale single and batch pointer
  overloads. Save-on-Exit accepted rows dirtied after its original snapshot,
  Discard accepted failed reloads, candidate setters could destroy an owner, and
  a path-changing save could leave duplicate activity identities when its owner
  disappeared at commit. Plan target changes left stale ranges/conflicts, partial
  exports were accepted, and Split did not reject in-place RideFile mutation.
- Resolution: Deletion now locks and snapshots every source, backup, and owned
  sidecar; copies and hashes the source into a same-directory fsynced staging
  file; preserves an existing backup under a transaction-specific recovery
  name; publishes and verifies the new backup; and only then moves and syncs
  the source tombstone. The model changes only after that durable storage
  commit. Rollback first restores, hashes, and syncs the source and will not
  remove the verified new backup until that invariant holds. It reconciles
  partial move states from the filesystem rather than trusting a failed return
  value. Planned and completed backups have separate namespaces, and the
  planned namespace is a checked real directory whose parent entry is synced
  before every use. Derived files are snapshot-validated and removed only
  after commit. Explicit per-item and batch results distinguish rejection,
  rollback, clean commit, cleanup pending, partial commit, recovery required,
  and not attempted. Legacy single-item booleans report logical removal while
  legacy batch booleans retain their historical any-success contract. DELETE
  processors run only after storage preconditions and staging succeed, with all
  exceptions converted to rejection. Linked peers must be reciprocal and
  clean. Every reentrant metadata/save boundary revalidates guarded target and
  peer identities; runtime rollback restores only the original peer identity
  and otherwise reports recovery with old and current paths. Batch requests
  snapshot guarded object identity before any signal. The batch coordinator
  also guards and revalidates its cache, context, athlete, model, and estimator
  ownership chain across every nested removal and refresh boundary; if an owner
  disappears, it preserves completed results and marks every remaining request
  not attempted. Reentrant model callbacks now remove the target at its current
  valid index and never move it into a row that disappeared during the signal.
  Linked deletion rejects a peer whose production save would change its
  filename; atomic filename-changing reciprocal publication is now completed
  by `DATA-021`.
  Every post-save boundary still requires the same guarded peer to remain
  uniquely cached in its original namespace with a regular, non-symlink source.
  Selection is made valid before `rideDeleted`, and the notifier no
  longer selects the removed object. Nested model callbacks re-resolve both the
  target row and selection before each removal signal; nested deletion is
  rejected while a model transaction is active. Staging cleanup failure is a
  recovery-required outcome that stops a batch, and single removal cancels an
  active refresh before processors or storage mutation. Legacy pointer overloads
  resolve unique live membership without dereferencing a stale pointer. Save
  transactions retain a successful same-path commit after finalization, skip
  `markClean` if the ride disappeared, and guard dialog instances, candidate
  setters, and activity identity across nested callbacks. A path-changing save
  whose owner disappears rolls back the new target so the old and new identities
  cannot coexist. Save-on-Exit repeatedly reconciles newly dirty and re-dirtied
  rows, and Discard succeeds only after a guarded reload. Any peer identity
  change observed during linked deletion requires recovery; the fixed-path
  deletion workflow never adopts a renamed peer.
  Repeat Plan requires a non-empty, freshly resolved copy set before target
  removal, its pages resolve owners through the guarded wizard, and the shared
  plan reader stores its context as `QPointer`. Plan selection and gap changes
  recalculate target times, ranges, and conflicts; linked conflicts use the
  target time, and partial exports fail while leaving the wizard open. Split
  snapshots both RideFile identity and its revision across modal and publication
  boundaries. RideCache callback continuations use a shared owner guard. UI
  callers surface cleanup and recovery outcomes; batch recovery stops and marks
  remaining rows not attempted.
- Verification: The focused removal program passes all 107 cases normally,
  under ASan/UBSan, and under ThreadSanitizer without race reports. Strict LSan
  is clean except for the two deliberately synchronous Qt model-destruction
  rows documented by `TEST-007`; those rows remain ASan/UBSan- and TSAN-clean.
  Atomic save passes 115 cases under strict ASan/UBSan/LSan and under TSAN.
  Split data and publication pass 28 and 33 cases, and the deletion, save,
  Repeat Plan, plan-reader lifetime, and cache-callback contract suites pass 28,
  15, 17, 7, and 3 cases under both sanitizer configurations. The complete Qt
  6.8.3 production application compiles and links, its disposable-HOME offscreen
  smoke test passes, and the 99-target offscreen matrix reports 3,661 passed,
  zero failed or blacklisted, and 12 expected Linux skips. The AppImage packaging
  policy check and compile-only SIP prerequisite pass. The changed Windows atomic
  file branch passes a MinGW64 C++17 syntax check; native Windows durability
  remains `DUR-014`.
- Residual: Ordinary process-crash reconciliation is tracked by `DUR-013`, plan
  replacement/import atomicity by `DATA-020`, Windows directory durability by
  `DUR-014`, hard-link portability by `PORT-001`, and non-cooperating pathname
  replacement by `SEC-025`.

### DATA-019: Linked metadata and activity deletion are separate transactions

- Status: FIXED
- Code: `src/Core/LinkedActivityRemovalJournal.cpp`,
  `src/Core/LinkedActivityRemovalJournal.h`, `src/Core/RideCacheRemoval.cpp`,
  `src/Core/RideCache.cpp`, `src/Core/Athlete.cpp`,
  `src/Gui/SplitActivityWizard.cpp`, and the focused removal, athlete-startup,
  and Split test projects
- Impact: Runtime rejection now restores the linked peer and reports a failed
  restoration, but the peer save necessarily precedes the source storage
  transaction. A process or power loss in that interval can persist a cleared
  peer while the still-live target retains its reciprocal link. A later manual
  recovery is then required even though every activity byte remains available.
- Test-first evidence: The original subprocess crash after the peer save left
  the survivor durably unlinked while the target source remained live. Added
  failpoints then exposed six non-idempotent cleanup interruptions. Independent
  RED regressions showed compensation serializing before the exact peer bytes
  were restored, startup recovery failure not reaching the athlete load-failure
  rollback, two live transactions acquiring the same athlete, abandoned
  journals allowing a successor transaction, and existing journal directories
  retaining broad permissions. Five oversized control-file rows also showed
  manifest, marker, and temporary files being opened before their limits were
  enforced. Four final crash rows initially exited normally because journal
  directory creation, `peer.old`, initial-manifest publication, and the peer
  file commit had no precise transition hooks.
- Resolution: Linked deletion now uses one private, athlete-root transaction
  namespace and a strict versioned JSON manifest containing root-relative roles,
  sizes, and SHA-256 snapshots for the source, prior backup, peer generations,
  and derived files. The journal separately preserves the exact prior peer and
  staged peer bytes. An athlete-wide process lease spans peer staging,
  the existing storage transaction, the durable `COMMITTED` decision marker,
  and cleanup; unresolved journals block a successor transaction. Before the
  marker, restart or runtime compensation restores the complete old pair and
  prior backup. After the marker, recovery completes the deletion, preserves the
  archived source, installs the unlinked survivor, and removes derived and
  transaction files. Every transition is file- and parent-directory-synced on
  Unix, snapshot-validated, idempotent across repeated recovery, bounded for
  journal control files, and fail-closed for unknown entries, symbolic links,
  path traversal, overlapping roles, or non-private transaction directories.
  RideCache reconciles synchronously before scanning activities; a failure
  aborts athlete publication through the normal load-failure signal. Linked
  archived removal and filename-changing peer saves are rejected before
  mutation, while Split can remove a source only after its link is removed or
  when `Keep original` is selected.
- Verification: The focused removal suite passes 147 cases normally. Its 41
  DATA-019 security, serialization, lease, startup, and crash/recovery cases pass
  under strict ASan/UBSan/LSan and under ThreadSanitizer without reports. Split
  data passes 31 cases and athlete/startup safety passes 116. The complete Qt
  6.8.3 application compiles and links, and its disposable-HOME offscreen
  version smoke test exits successfully. The 99-target offscreen matrix reports
  3,705 passed, zero failed or blacklisted, and 12 expected Linux skips; its
  AppImage packaging policy and compile-only SIP targets also pass.
- Residual: Ordinary unlinked deletion restart recovery remains `DUR-013`;
  Windows directory durability remains `DUR-014`; non-cooperating pathname
  replacement remains `SEC-025`; production linked-deletion JSON persistence
  coverage remains `TEST-006`; and the peer writer's extra full-payload buffer
  is tracked by `PERF-012`.

### DATA-021: Filename-changing linked saves are not one transaction

- Status: FIXED
- Code: `src/Core/LinkedActivitySaveJournal.cpp`,
  `src/Core/LinkedActivitySaveJournal.h`,
  `src/Core/LinkedActivityRemovalJournal.cpp`, `src/Core/RideCache.cpp`,
  `src/Gui/MainWindow.cpp`, `src/Gui/MainWindow.h`,
  `src/Gui/SaveDialogs.cpp`, `src/Gui/SaveDialogs.h`, and the focused atomic
  save and RideCache-removal test projects
- Impact: Save preflight updates both reciprocal filenames in memory and then
  saves the activity set sequentially. If one conversion or rename commits and
  a later peer save fails, the successful file can persist a link to a filename
  that was never published. The caller reports failure, but there is no rollback
  to one complete old or new linked pair. Linked deletion now rejects a peer
  whose save would change its filename, but ordinary linked save workflows still
  had this runtime partial-save defect; it was distinct from the fixed deletion
  crash window in `DATA-019`.
- Test-first evidence: The original real-JSON regression failed after the first
  renamed peer was published and the second writer failed, leaving a successful
  prefix instead of one complete generation. A second RED test showed that the
  first activity was serialized before the second activity's save processor
  could mutate it. Security review then produced two RED rows in which
  unreadable, oversized `.manifest.json.*.tmp` and `.COMMITTED.*.tmp` files were
  opened before their 4 MiB and 128-byte limits were checked. Two further RED
  rows showed that pre-manifest recovery accepted an embedded
  `manifest.json` substring and a malformed manifest-temporary suffix as data,
  then silently removed them. All four defects were fixed without weakening
  their tests.
- Resolution: Filename-changing reciprocal saves now use a private
  `.gc-transactions/linked-save/<uuid>` journal. A strict versioned manifest
  records root-relative source, target, and backup roles plus exact sizes and
  SHA-256 digests for the old source, prior backup, and staged new generation.
  The save and removal journals share one athlete-wide lease, reject unresolved
  work in either namespace, and hold the complete source/target/backup lock
  graph. Every requested save processor runs and is revalidated exactly once
  before any activity is serialized; only after every staged JSON and manifest
  update is durable does publication begin. Before the synced `COMMITTED`
  marker, failure or restart restores the complete old generation and prior
  backups. After the marker, recovery installs every staged target, publishes
  conversion backups, retires every superseded source, and then removes the
  journal. Recovery is idempotent and fail-closed for malformed schemas,
  traversal, overlapping roles, symlinks, unknown entries, changed production
  files, oversized control files, unsafe permissions, or incomplete staging.
  RideCache reconciles removal and save journals before indexing activities and
  exposes a recovery failure through `startupRecoveryError`. Save preflight can
  resolve a peer by either its current or predicted new filename, and Save on
  Exit updates both rows' completed identities after one linked batch commit.
- Verification: The focused atomic-save suite passes 193 cases normally, under
  strict ASan/UBSan/LSan, and under ThreadSanitizer. Its subprocess matrix covers
  18 ordinary rename transitions and four conversion-specific transitions,
  including prior-backup capture, backup publication, source retirement, and
  the commit marker; every recovery is repeated and yields one reciprocal old
  or new pair. The removal/startup suite passes 151 cases normally and under
  ThreadSanitizer. Its DATA-021-specific rows are strict-LSan-clean; the complete
  ASan/UBSan run passes functionally and retains only the documented 256-byte Qt
  model-destruction leak in `TEST-007`. Existing save/deletion workflow, Split
  data/publication, and athlete startup suites pass 15, 28, 31, 33, and 116
  cases. The complete Qt 6.8.3 application compiles and links, both its
  disposable-HOME offscreen version check and timed event-loop startup succeed,
  and all changed production translation units pass a MinGW64 C++17 syntax
  check.
- Residual: Windows cannot durably sync directory entries (`DUR-014`), and a
  non-cooperating process can still replace a checked pathname outside the
  cooperative lock protocol (`SEC-025`). `TEST-006` continues to track the
  separate production linked-deletion persistence fixture; this fix does cover
  ordinary linked-save persistence with real reciprocal JSON files.

### DATA-020: Plan replacement and import are not all-or-nothing

- Status: FIXED
- Code: `src/Gui/PlanWizards.cpp`, `src/Planning/PlanBundle.cpp`,
  `src/Planning/PlanBundleImportJournal.cpp`,
  `src/Planning/PlanReplacementJournal.cpp`, `src/Core/RideCache.cpp`,
  `src/Core/RideCacheRemoval.cpp`, `src/FileIO/AtomicFileWriter.h`, and
  `src/Train/TrainDB.cpp`
- Impact: Bundle import removed conflicts and published workout-library and
  TrainDB state in separate steps. A copy, parse, database, or process failure
  could therefore leave only a prefix of the imported bundle installed.
  Repeat Plan previously had the same failure mode, but its replacement path is
  now generation-atomic.
- Test-first evidence: The original import source still called destructive and
  per-activity publication APIs instead of one generation replacement. RED
  parser rows accepted dot components, Windows device names, trailing dots,
  and colon-bearing workout names. TrainDB accepted malformed auxiliary
  indexes and a second pending import for another athlete. The durable journal
  API and restart coordinator were absent. A final injected SQLite case
  reproduced the uncertain-commit boundary: the decision was durable, but the
  API reported it as definitely uncommitted and would have allowed the staged
  plan journal to be rolled back. Final source-level regressions also force a
  target substitution after publication validation but before SQLite work and
  require the legacy unbound recovery callback to fail closed. Restart cleanup
  also substitutes a different predecessor generation with identical bytes and
  requires that generation to be preserved.
- Resolution: PlanBundle now reopens and validates every selected activity and
  attached workout before mutation, bounds individual and aggregate payloads,
  verifies both the bundle MD5 reference and stable exact contents, rejects
  non-portable target components, and submits one complete activity generation
  to RideCache. New workout bytes and SHA-256 identities are stored with a
  globally serialized durable decision in auxiliary TrainDB tables. RideCache
  commits that decision after all plan files are staged and never rolls the
  plan journal back once the database commit may be durable. It then publishes
  one plan generation, idempotently publishes locked workout files, and inserts
  all workout rows while retiring the decision in the same database
  transaction. Recovery now accepts only a bound completion callback, verifies
  that its published-target validator runs while the same `TrainDB` LUW is
  active, and invokes that validator immediately before journal retirement.
  Overwrite decisions persist the predecessor's native generation fingerprint
  with its size and digest, and restart cleanup requires all three before an
  anchored deletion.
  Startup completes this outer transaction before ordinary plan
  journal reconciliation. Corrupt payloads, schemas, roots, identifiers,
  conflicting targets, and incomplete database completion fail closed and
  retain recovery state. The import UI distinguishes an uncommitted failure
  from a committed operation that requires restart recovery.
- Verification: Strict ASan/UBSan/LSan runs pass all 30 TrainDB cases, 20
  PlanBundle reader/source-contract cases, six bundle-import journal cases, and
  113 plan-replacement journal cases. The complete 325-case RideCache removal
  and PlanBundle integration program passes ASan/UBSan; its documented Qt model
  teardown leak rows run with LSan disabled as tracked by `TEST-007`. The six
  bundle-import journal, 113 plan-journal, and 325 RideCache cases also pass
  ThreadSanitizer without a race report. A clean full normal matrix reports 101
  QtTest result blocks, 4,135 passes, zero failures or blacklisted cases, and 12
  expected platform skips. The complete production application compiles and
  links, then remains healthy in a 20-second offscreen smoke test with isolated
  HOME and XDG directories.
- Residual: Real widget interaction remains tracked by `TEST-005`, production
  reciprocal JSON persistence by `TEST-006`, and the adversarial Qt model leak
  rows by `TEST-007`. Windows directory-entry durability remains `DUR-014`, and
  a non-cooperating process can still replace a checked workout pathname outside
  the cooperative lock protocol as tracked by `SEC-025`.

### MEM-024: RideCache callbacks exposed destroyed activity addresses

- Status: FIXED
- Code: `src/Core/RideCache.cpp`, `src/Core/RideCache.h`,
  `src/Core/RideCacheRemoval.cpp`, `src/Core/RideCacheLiveView.cpp`,
  `src/Core/RideCacheGarbageCollection.cpp`, `src/Core/RideCacheModel.cpp`,
  `src/Core/RideCacheModelProtocol.cpp`, `src/Core/RideItem.cpp`, and
  `src/Gui/NavigationModel.cpp`
- Impact: `RideCache` and its Qt model shared raw `RideItem*` vectors. A direct
  callback deletion could leave an address visible to removal preflight,
  sidecar ownership scans, startup snapshots, background refresh, model data,
  selection, garbage collection, and navigation history. Those paths could
  dereference freed memory, publish a dangling item, or delete the same object
  again. Nested reset/insert/remove notifications could also violate Qt's model
  protocol while the vectors were being changed.
- Test-first evidence: An ASan RED regression deleted a cached item but retained
  its address, then reproduced a heap-use-after-free in
  `RideItem::getLinkedFileName()` from `RideCache::checkRemovalLinks()`. A second
  RED row put a destroyed unrelated item ahead of plan replacement; replacement
  rejected the otherwise valid generation and teardown attempted to delete the
  freed address. Model-signal rows then covered target and sibling destruction
  before and after removal notifications, readers invoked inside those signals,
  garbage collection, startup invalidation, selection neighbors, deferred
  deletion, owner destruction, and a destroyed navigation-history item. A final
  RED row showed a refresh requested during plan replacement leaking into the
  next explicitly no-refresh deletion.
- Resolution: Cache-owned destruction now records a set tombstone and
  invalidates startup snapshots, while temporary `RideItem` destruction does
  neither. Public readers return an implicitly shared live snapshot; the model
  alone retains physical-row access and returns no data for a tombstone.
  Removal and replacement stop workers before callbacks, reject stale inputs,
  skip dead addresses, purge destroyed rows under a balanced reset, and defer a
  single refresh until the mutation finishes; replacement blocking is resolved
  before ordinary-removal refresh deferral so that state cannot leak into the
  next operation. Model changes use explicit
  reset/insert/remove frames, reject incompatible reentrancy, nest compatible
  work under an outer reset, and defer configuration resets. Navigation history
  stores `QPointer<RideItem>`, and garbage collection never schedules an already
  destroyed address.
- Verification: The complete 231-case removal/model program passes ASan/UBSan
  and ThreadSanitizer. The newly isolated liveness rows pass strict LSan. The
  only remaining leak is the unchanged Qt 6.8.3 interrupted-removal bookkeeping
  documented by `TEST-007`.
- Residual: Non-removal mutations that encounter a tombstone remain tracked by
  `MEM-025`; post-I/O model publication failures remain `DATA-022`.

### THREAD-018: Estimator cancellation raced its worker and did not join

- Status: FIXED
- Code: `src/Metrics/Estimator.cpp`, `src/Metrics/Estimator.h`, and
  `src/Metrics/EstimatorThreadControl.h`
- Impact: The GUI thread wrote a plain `bool abort` while the estimator worker
  read and cleared it without synchronization. `stop()` polled `isRunning()` and
  the same flag instead of joining, so destruction or cache mutation could
  continue while the worker still held raw activity addresses. The accesses
  were a C++ data race even when the polling appeared to work.
- Test-first evidence: The extracted lifecycle contract runs a cooperative
  worker until cancellation, requires the request to remain asserted through
  worker exit, verifies `stop()` waits for that exit, and then starts a second
  worker after both cancellation and natural completion.
- Resolution: `EstimatorThreadControl` publishes cancellation with an atomic
  release/acquire flag and joins with `QThread::wait()`. The request is cleared
  only after the worker has stopped or immediately before a verified new start.
  `Estimator::~Estimator()` calls `stop()`, pending lazy-start timers are
  cancelled, and long sport, ride, week, model, filtering, and publication loops
  contain cooperative cancellation checkpoints.
- Verification: Both lifecycle regressions pass strict ASan/UBSan/LSan and
  ThreadSanitizer. The full 231-case cache/removal TSan program also verifies
  that estimator shutdown precedes model mutation without a race report.

### MEM-025: Non-removal cache mutations can sort tombstoned RideItems

- Status: FIXED
- Code: `src/Core/RideCacheCalendarMutations.cpp`,
  `src/Core/RideCacheImport.cpp`, `src/Core/RideCacheMutationScope.cpp`,
  `src/Core/RideCacheBulkMerge.h`, and `src/Metrics/Estimator.cpp`
- Impact: A callback can directly destroy a cache-owned activity and leave its
  address tombstoned in the physical model vector. Removal and plan replacement
  purge that state, but `addRide()`, `addRides()`, `moveActivity()`, planned
  copy, and planned shift still sort or index the raw vector. The comparator or
  bulk-merge key extractor can therefore dereference the destroyed address. An
  estimator generation started before such a callback can also retain the same
  raw address until a later refresh calls `stop()`.
- Evidence: `addRide()` skips a tombstone in its duplicate scan and then passes
  the complete raw vector to `std::sort`; `addRides()` calls `keyFor()` for every
  raw row before it attempts a model reset. The move/copy/shift publication
  paths likewise sort without first calling `purgeDestroyedModelRows()`, and
  their callbacks do not share the removal path's worker-quiescence lease.
- Test-first evidence: The initial ASan regression retained a destroyed cache
  row and reproduced a heap-use-after-free in the next import. Direct
  regressions for move, single and batch planned copy, and planned shift first
  failed to link because those production methods were outside the testable
  cache module. Once linked, the batch-copy row failed because the operation
  passed an invalid target time. The final action regressions destroy an
  unrelated row from `modelAboutToBeReset` and require publication to retain
  only live, sorted rows. Import rows cover tombstones before and during reset,
  and repeated operations require one coalesced background resume.
- Resolution: Every affected mutator now acquires `RideCacheMutationScope`,
  which blocks reentrant cache operations, joins cache refresh and estimator
  work, purges tombstones, and queues one generation-checked resume.
  `resetAndSort()` owns the balanced model reset and removes rows destroyed both
  before and during publication before sorting. Bulk merge begins its reset
  before key extraction. Move, planned-copy, and planned-shift implementations
  share the coordinator in `RideCacheCalendarMutations.cpp`; all inputs and
  unpublished copies are guarded through callbacks. Batch copy also preserves
  each source activity's time when constructing its target filename.
- Verification: Nine focused lifecycle and action regressions pass strict
  ASan/UBSan/LSan. The complete 240-case RideCache program passes ASan/UBSan and
  ThreadSanitizer, and all 13 bulk-import cases pass both sanitizer
  configurations. The production application links and survives an isolated
  ten-second offscreen event-loop smoke test. Durable post-I/O publication
  atomicity remains tracked separately as `DATA-022`.

### DATA-022: Model publication can fail after activity files were changed

- Status: FIXED
- Code: `src/Charts/CalendarWindow.cpp`, `src/Core/RideCache.cpp`,
  `src/Core/RideCacheCalendarMutations.cpp`, `src/Core/RideCacheMutationScope.cpp`,
  `src/Core/RideCacheModel.cpp`, `src/Core/RideCacheModelProtocol.cpp`,
  `src/Core/RideCacheRemoval.cpp`, and
  `src/Planning/PlanReplacementJournal.cpp`
- Impact: The old move and planned-shift paths performed multiple durable file
  transitions without a restart-recoverable generation. A process exit could
  split an activity, its derived files, and its reciprocal link between old and
  new identities. A runtime owner loss after durable commit was also flattened
  into an ordinary failure, encouraging an unsafe retry after storage changed.
- Evidence: The previous `moveActivity()` renamed files and then rewrote the
  activity before publishing its cache identity. Planned shift repeated that
  sequence for each item. Neither runtime rollback was journaled or
  restart-idempotent, and the calendar saved affected rows again after a
  successful operation.
- Test-first evidence: Four RED regressions opened an incompatible insert frame
  from a storage callback after move, single copy, batch copy, and shift had
  written their target files. Every insert was accepted, the later reset was
  rejected, and each operation returned with durable state that its cache model
  had not published. A later batch-copy RED left its first target and cache row
  published when staging the second copy failed. A stale-source regression also
  produced a UBSan invalid member access in the new wrapper before its cache
  membership check; both now fail closed without changing storage.
- Model reservation: `RideCacheMutationScope` acquires a tokenized model
  reservation after quiescing workers and before storage work begins. Public
  insert, remove, and reset attempts cannot consume the reserved publication
  boundary; only the scope can begin its balanced reset. Configuration resets
  are deferred until release, and the reservation is released on every scope
  exit. All four calendar mutations therefore publish or reject before touching
  storage instead of encountering a busy model afterward.
- Planned-copy resolution: Single and batch planned copies stage every
  transformed target through `PlanReplacement::Journal` and publish one fsynced
  generation while the model reservation is held. A staging failure rolls the
  entire batch back. Startup reconciliation selects the complete old or new
  generation after process termination at each directory, manifest, staging,
  target, commit-marker, and cleanup transition. The replacement/import path
  now uses the same reserved mutation scope, explicit target times remain
  supported, stale sources are rejected before dereference, and the obsolete
  direct `QFile::copy` implementation was removed.
- Move and shift resolution: `moveActivity()` and the complete planned-shift
  batch now use one `PlanReplacement::Journal` generation. It stages the
  rewritten activity, every existing CPX/CPI/notes sidecar, and an in-place
  rewrite of a validated reciprocal linked peer. Overlapping shift source and
  target paths are supported. Cache identities are changed only after the
  durable commit marker exists, and the queued mutation refresh repairs a
  post-commit cache-owner loss from the committed files.
- Result contract: Calendar operations report `committed`, `cacheUpdated`,
  `cleanupComplete`, and warnings separately. A committed but degraded result
  tells the user not to repeat the operation and refreshes the activity view.
  The calendar no longer performs a second non-atomic save after move, shift,
  or copy.
- Verification: Twelve focused identity/result cases pass strict
  ASan/UBSan/LSan. The complete 317-case RideCache program passes ASan/UBSan and
  ThreadSanitizer, including process-exit/restart matrices for unlinked move,
  linked move, batch copy, and overlapping shift at every journal transition
  with repeated recovery. All 22 staging and 112 journal cases pass strict
  ASan/UBSan/LSan. The production application links and survives an isolated
  15-second offscreen event-loop smoke test with a disposable home directory.

### DUR-013: Activity deletion recovery files are not reconciled on restart

- Status: FIXED
- Code: `src/Core/LinkedActivityRemovalJournal.cpp`,
  `src/Core/LinkedActivityRemovalJournal.h`,
  `src/Core/RideCacheRemoval.cpp`, and
  `unittests/Core/rideCacheRemoval/testRideCacheRemoval.cpp`
- Impact: `DATA-019` journaled and reconciled linked deletion, but ordinary
  unlinked deletion previously retained only verified canonical and
  transaction-specific recovery copies. A process exit between its durable
  transitions has no result object, and startup does not classify its
  `.gc-copy-*`, `.gc-previous-*`, or `.gc-remove-*` files. Recoverable data can
  therefore remained orphaned and required manual inspection.
- Test-first evidence: A subprocess stopped immediately after the ordinary
  source was durably tombstoned. Reopening the same athlete returned without a
  startup error but left the source missing instead of restoring its newest
  contents and the pre-existing backup generation. The final matrix terminates
  ordinary deletion at journal creation, manifest publication, backup staging,
  previous-backup preservation, backup publication, source tombstoning, the
  commit marker, every cleanup occurrence, and every journal cleanup boundary.
- Resolution: The existing linked-removal journal now has a version-2 manifest
  with an explicit optional-peer role. Ordinary archived deletion prepares the
  same fsynced role-hash manifest with no peer, uses its transaction id for all
  storage sidecars, records the commit decision only after the source is safely
  archived, and retains committed cleanup work for startup. Restart recovery
  deterministically rolls an uncommitted generation back or a committed
  generation forward and removes all verified sidecars. Version-1 linked
  manifests remain accepted. Pre-manifest validation now precedes journal
  directory creation, and absent derived files may safely name a missing parent
  without attempting to create a lock there.
- Verification: The 15-transition ordinary crash/restart matrix, the existing
  linked crash/restart matrix, and the version-1 compatibility regression pass.
  The complete 341-case RideCache program passes normal, ASan/UBSan, and
  ThreadSanitizer builds. The repository-wide normal matrix reports 101 QtTest
  result blocks, 4,151 passes, zero failures, 12 expected skips, and zero
  blacklisted cases. The production application links and survives an isolated
  20-second offscreen event-loop smoke test with a disposable home directory.
- Residual: Native Windows directory-entry durability remains `DUR-014`, and
  non-cooperating pathname replacement remains `SEC-025`.

### DUR-014: Windows deletion does not durably sync directory entries

- Status: FIXED
- Code: `src/FileIO/AtomicFileWriter.h`,
  `src/Core/LinkedActivityRemovalJournal.cpp`,
  `src/Core/RideCacheRemoval.cpp`,
  `unittests/FileIO/durableFilesystem/`, and
  `.github/workflows/windows-durable-filesystem.yml`
- Impact: `syncParentDirectory()` is a successful no-op outside Unix. Windows
  file handles are flushed and `MoveFileExW` uses `MOVEFILE_WRITE_THROUGH`, but
  planned-backup directory creation and all directory-entry cleanup transitions
  lack the durability barrier that the deletion protocol assumes. A power loss
  can therefore violate the documented committed/rolled-back state even when
  runtime calls all returned success.
- Test-first evidence: The new standalone durability regression initially
  failed to compile because there was no durable directory-create, file-remove,
  or directory-remove API. The old deletion implementation instead called
  `QDir::mkpath()`, `QFile::remove()`, or `QDir::rmdir()` and then accepted the
  Windows no-op `syncParentDirectory()` result.
- Resolution: Windows no longer claims a directory-handle `fsync` equivalent.
  New directory entries are created under a UUID staging name and published by
  `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`. Files and empty directories are
  opened without following the final reparse point, checked by handle, and
  removed with `SetFileInformationByHandle(FileDispositionInfo)` on a
  `FILE_FLAG_WRITE_THROUGH` handle. UUID directory staging left by termination
  inside the journal namespace is a valid empty pre-manifest journal and is
  removed by startup reconciliation. Unix uses the same helpers with its
  existing parent-directory `fsync`. Planned-backup creation, journal namespace
  and instance creation, rollback, committed cleanup, and journal removal now
  use these helpers. Recovery-copy creation also publishes through the existing
  atomic writer instead of creating its final pathname directly.
- Verification: The focused test passes 17 cases on Linux, including strict
  ASan/UBSan/LSan, and 18 cases under native Windows MSVC in GitHub Actions. The
  Windows matrix terminates the subprocess after staging-directory creation,
  write-through publication, delete disposition, handle close, and the logical
  parent barrier for file and directory operations. The complete 341-case
  RideCache removal program passes normal, ASan/UBSan, and ThreadSanitizer
  builds; the 193-case atomic-save program also passes its normal build. The
  complete Linux repository matrix passes all 102 test programs: 4,168 passed,
  0 failed, 12 skipped, and 0 blacklisted. The complete application builds and
  remains alive until the expected 20-second timeout in an isolated, networkless
  offscreen smoke test with a disposable home directory.
- Residual: Hosted CI validates process termination rather than cutting power to
  a physical storage device. Persistence therefore relies on the documented
  Windows write-through contract and the device honoring flushes. Parent-path
  replacement by a non-cooperating process remains `SEC-025`.

### DUR-015: Failed planned-backup directory sync was not retried

- Status: FIXED
- Code: `src/Core/RideCacheRemoval.cpp` and
  `unittests/Core/rideCacheRemoval/testRideCacheRemoval.cpp`
- Impact: The planned backup namespace was parent-synced only when its directory
  was first created. If that sync failed, the directory remained present, so a
  retry skipped the durability barrier and could proceed as though the directory
  entry had been committed. A power loss could then lose the namespace after an
  apparently successful activity deletion.
- Test-first evidence: A failpoint that rejected two consecutive sync attempts
  showed the second removal reaching the transaction because the existing
  directory suppressed the retry.
- Resolution: Every planned deletion now validates the real directory and syncs
  its parent entry before using the namespace, regardless of whether this call
  created the directory. The regression rejects both injected failures without
  changing the source and commits only after the third, successful sync.
- Verification: Included in the focused removal program and its final normal and
  sanitizer runs recorded under `DATA-018`.

### SEC-025: Activity transaction pathname checks remain TOCTOU-prone

- Status: FIXED
- Code: `src/Core/RideCacheRemoval.cpp`,
  `src/Core/CredentialSettings.cpp`,
  `src/Core/LinkedActivitySaveJournal.cpp`,
  `src/Core/LinkedActivityRemovalJournal.cpp`,
  `src/Planning/PlanReplacementJournal.cpp`,
  `src/FileIO/AnchoredFileSystem.cpp`, and `src/FileIO/AtomicFileWriter.h`
- Impact: Unsafe names, final symlinks, and planned-backup symlinks are rejected,
  snapshots are hashed, and cooperative writers share path locks. A separate
  process running as the same OS identity can still replace some parent or
  directory entries between pathname checks and operations. Remaining
  path-based mutation and namespace-bootstrap paths can therefore observe a
  different generation or act outside the intended athlete namespace.
- Test-first evidence: Deterministic regressions replace snapshotted activity
  parents, final entries, active journal directories, `manifest.json`,
  `peer.old`, commit markers, and enumerated journal temporary files at the
  corresponding validation-to-mutation boundaries. Further regressions replace
  the entire linked-removal namespace with a symlink immediately before journal
  cleanup, exchange a just-verified empty directory before `rmdir`, substitute
  the linked-save namespace and newly created journal, and terminate after a
  private staging directory is anchored but before publication. The unsafe
  baselines accepted byte-identical substitutes, overwrote or deleted them,
  followed a redirected namespace far enough to remove an outside directory,
  or left an unrecognized staging name that blocked later recovery.
  Plan-specific follow-up regressions replace a byte-identical manifest,
  commit marker after publication and while being read, staged activity,
  preserved old activity, and pre-manifest recovery file at their respective
  observation-to-use boundaries. Another exchanges the newly created UUID
  journal before its first old-copy publication. The unsafe baselines accepted
  the replacement generations, published from them, deleted them, or wrote a
  new journal file into the substituted directory. Two additional lifetime
  regressions exchange a just-created old copy before manifest publication and
  a just-recorded staged file before the record operation returns; both unsafe
  baselines accepted the substituted generation. A staged-set rollback
  regression then moves a pinned second input aside, installs a foreign file at
  its old name, and injects a publisher failure. The unsafe cleanup deleted the
  foreign replacement. A follow-up contract regression showed that the first
  identity-bound draft retained an otherwise removable stage when no publisher
  was supplied.
- Resolution: RideCache storage transactions now open one anchored
  athlete-root generation, walk children without following links, and pin each
  existing source, backup, derived file, and journal control file once. Reads,
  no-replace publication, rollback, and cleanup operate through those pinned
  objects and anchored parents. Ambiguous post-publication states require
  recovery instead of treating equal bytes as identity proof. Atomic writers
  revalidate the expected old generation immediately before publication and
  use identity-bound Windows handles for replacement and deletion.

  Linked-removal journals now retain their namespace parent and journal child
  anchors as one generation. Unix cleanup first moves an empty journal to a
  random valid-UUID quarantine with a no-replace rename, verifies the held
  `(device, inode)` twice, removes only the verified quarantine name, checks
  both quarantine and original names, and synchronizes the anchored parent.
  Linux additionally requires `st_nlink == 0`; macOS uses the successful
  `rmdir`, held identity, and anchored-name checks because APFS reports one
  directory link before and after removal. Windows exchanges the observation
  handle for an identity-checked delete handle, restores a normal observation
  handle after sharing or nonempty-directory failures, and treats any
  repopulated UUID name as partial cleanup. A partial quarantine remains
  visible to both rollback and commit retries instead of being mistaken for a
  completed transaction.

  Linked-save publication, rollback, source retirement, commit-marker handling,
  and cleanup now retain pinned identities and anchored parents through their
  mutations. Create-new atomic writers similarly hand their staging identity
  from the writer to the anchored publisher, and stale `QLockFile` removal
  helper names are parsed and ignored without weakening rejection of unknown
  entries.

  A new private-child primitive creates a random recovery-owned staging
  directory below an anchored private parent, hardens and verifies it through
  its open identity, publishes it with no replacement, synchronizes the parent,
  and returns an open anchor for the published generation. Unix requires the
  effective owner, exact mode `0700`, and no Linux or macOS extended ACL; it
  uses descriptor-relative creation and identity checks. Windows creates an
  owner-only protected inheritable DACL at birth, validates persistent ACL
  support, keeps identity-checked handles across publication, and denies delete
  sharing during the mutation. Cleanup failures retain their verified recovery
  location and report a partial effect. Crash-left staging names are plain UUIDs
  so existing pre-manifest recovery can reconcile them.

  Linked-save preparation now hardens `.gc-transactions` and `linked-save`
  through directory anchors and creates each UUID journal with that primitive.
  Recovery hardens the same parent, namespace, and every valid UUID child before
  namespace enumeration, manifest-existence branching, child enumeration, or
  manifest reads. Preparation verifies the anchored journal after creation
  hooks, before every source and prior-backup copy, and before manifest
  publication. Substitutions injected at those deterministic validation hooks
  are rejected without writing the next transaction file into the substitute.
  Readiness now retains the linked-save, linked-removal, and plan-replacement
  namespace generations together, performs bounded anchored enumeration, and
  rechecks every held name before journal creation. Linked-save recovery uses
  two matching anchored namespace snapshots before mutation, opens each UUID
  child only when its native identity matches the enumerated generation, and
  performs another stable namespace snapshot after recovery. A journal replaced
  immediately after enumeration is rejected without deleting either the
  original or substitute generation.

  Linked-removal preparation and recovery now anchor the athlete root, create or
  open `.gc-transactions` and `linked-removal` as fixed private children, and
  create each UUID journal with the private-child primitive. Recovery performs
  bounded, repeated namespace enumeration before mutation and hands manifest,
  peer, temporary-file, and commit-marker identities through validation and
  cleanup. Its transient manifest-existence reference is released before
  Windows exchanges the journal observation handle for a delete handle.
  Reapplying an already exact Windows private ACL is observational, preserving
  the pinned child generation instead of changing its `ChangeTime`.

  Plan-replacement preparation uses the same anchored fixed-child bootstrap and
  private UUID-journal creation. This gives directories an explicit current-user
  owner even under an elevated Windows token. Readiness now retains all three
  activity-transaction namespace generations and enumerates them through their
  anchors. Recovery retains the athlete root, transaction parent, and plan
  namespace, requires two matching bounded snapshots before mutation, binds each
  opened UUID child to the enumerated native identity, and verifies a stable
  final namespace snapshot. Its standalone tests now link the anchored
  implementation, and native CI builds and runs the plan-replacement and
  plan-bundle suites on both Windows and macOS.

  Plan UUID journals now retain their namespace and child anchors through
  manifest, commit-marker, data-file, recovery, and cleanup operations. Initial
  manifests, preserved old copies, and commit markers are created directly
  below the anchored directory with no replacement. Existing manifests and
  marker reads are pinned to one native identity; manifest rewrites retain the
  expected old generation across pre-commit validation and re-pin the published
  generation. Staged and preserved activity files are obtained from a bounded,
  repeated anchored directory snapshot, pinned by the enumerated identity, and
  streamed from that handle into the atomic activity writer. The reusable
  anchored stream API verifies the file before, during, and after chunked reads
  without buffering a complete activity in memory.

  Cleanup removes only the pinned identities, rejects repopulated names, and
  removes the verified empty UUID child through the anchored quarantine
  primitive. Pre-manifest crash recovery follows the same bounded enumeration,
  pinning, stable-snapshot, and anchored-removal contract. Successful cleanup is
  recorded explicitly instead of inferred from a later pathname absence, so
  repeated cleanup remains idempotent without accepting a replaced journal.

  Staged-set publication now anchors and pins every valid staging generation
  before target locking or publisher callbacks. Publication revalidates each
  held source and both parent generations immediately before use. Cleanup
  removes only a staging name that still resolves to its pinned identity and
  synchronizes the held parent; substitutions, symlinks, and unpinnable entries
  are retained and reported. The missing-publisher path preserves its prior
  cleanup contract through the same identity-bound machinery, and split-output
  staging failures use a stop-before-publication discard path instead of raw
  pathname deletion.

  A published output is accepted as transaction-owned only when a newly pinned
  target has the same native identity, size, and digest as its staged source.
  Rollback removes that held identity through the anchored removal primitive. A
  target exchanged during finalization, or an output whose publisher cannot
  prove identity continuity, is retained and reported instead of being deleted
  by pathname.

  Generic replace-existing publication now uses the same observed-generation
  contract. First-time credential files use create-new publication and existing
  files use replace-existing publication through the configured writer factory;
  the generic writer interface exposes its staging path so platform hardening is
  preserved. A successful replacement is not reported when retirement of the
  displaced old target fails. On POSIX, an ambiguous post-exchange state fails
  closed and retains a verified recovery path instead of attempting a second,
  identity-unverified exchange. A custom writer whose commit cannot prove
  identity continuity reports failure and retains the uncertain output rather
  than restoring or deleting a pathname that may now name another file.
- Verification: Every deterministic regression failed for its intended unsafe
  behavior before its fix. On Linux, RideCache passes 390 cases with one skip,
  PlanReplacement 124, PlanBundleImport 8, and linked-save cleanup 4 under both
  ASan/UBSan and ThreadSanitizer; the anchored suite passes 83 cases with 13
  platform skips under both configurations. The release application also builds,
  links, and answers `--version` in the constrained remote Docker environment.
  Final-head hosted run `30956053366` passes both native jobs. Windows reports
  376/0/15 for RideCache, 85/0/22 for AnchoredFilesystem, 10/0/0 for linked-save
  cleanup, 121/0/3 for PlanReplacement, and 8/0/0 for PlanBundleImport. macOS
  reports 390/0/1, 81/0/15, 4/0/0, 124/0/0, and 8/0/0 respectively, plus the
  307/0/1 atomic-activity suite. The linked-save readiness and recovery
  regressions first reproduced the hidden pending namespace and destructive
  enumerated-child substitution. The resulting atomic-activity suite passes
  310/0/0 normally; 14 focused readiness, recovery, lock-guard, and hardened-
  journal cases pass under strict ASan/UBSan/LSan and ThreadSanitizer. The two
  plan-namespace regressions likewise first reproduced hidden sibling recovery
  and destructive enumerated-child substitution. PlanReplacement now passes
  126/0/0 normally, under strict ASan/UBSan/LSan, and under ThreadSanitizer;
  PlanBundleImport passes 8/0/0 in all three configurations. The nine new plan
  identity regressions first reproduced
  acceptance, publication, destructive cleanup, or out-of-generation writes.
  PlanReplacement now passes 136/0/0 normally, under strict ASan/UBSan/LSan,
  and under ThreadSanitizer. AnchoredFilesystem passes 84/0/13 in all three
  configurations, including direct multi-chunk and consumer-failure coverage
  for the pinned stream API. PlanBundleImport passes 8/0/0 normally and under
  both sanitizer configurations. The complete production application links,
  and an isolated minimal-platform `--version` smoke test reports
  `GoldenCheetah V3.8-DEV2605 (5012)`. The staged-input regressions first
  reproduced destructive replacement cleanup and the missing-publisher cleanup
  regression. Atomic activity save now passes 313/0/0 normally, under strict
  ASan/UBSan/LSan, and under ThreadSanitizer. Split activity save passes 33/0/0
  under both sanitizer configurations, including its staging-failure cleanup
  path.
  The final credential suite reports 426/0/7 normally, under strict
  ASan/UBSan/LSan, and under ThreadSanitizer. AnchoredFilesystem reports 88/0/13
  in all three configurations, with its five closure regressions passing in
  focused runs. Atomic activity save reports 317/0/0 in all three
  configurations, including 18 focused generic-publication regressions. Hosted
  runs `30982140513`, `30982140602`, and `30982140674` all pass on SEC-025
  implementation head `ef4dcac`, covering durable and anchored filesystems,
  native Windows and macOS activity transactions, and the complete build.
- Residual: The private-directory API explicitly trusts processes running as
  the same OS identity and privileged administrators. POSIX exposes no portable
  identity-conditional `rmdir`; private random quarantine, repeated identity
  checks, anchored post-checks, and fail-closed recovery bound but cannot remove
  its final check-to-syscall interval. A reported verified recovery path is
  point-in-time evidence while its identity is held, not a permanent claim after
  handles are released. These are explicit trust and platform limits; ambiguous
  states are surfaced for recovery and are no longer resolved by destructive
  pathname rollback.

### GUI-007: Modal activity workflows retained dangling RideItem pointers

- Status: FIXED
- Code: `src/Core/RideCache.cpp`, `src/Core/RideCacheCallbackGuard.h`,
  `src/Gui/SaveDialogs.cpp`, `src/Gui/SaveDialogs.h`,
  `src/Gui/MainWindow.cpp`, `src/Charts/CalendarWindow.cpp`,
  `src/Gui/BatchProcessingDialog.cpp`, `src/Gui/PlanWizards.cpp`,
  `src/Gui/PlanWizards.h`, `src/Gui/SplitActivityWizard.cpp`,
  `src/Gui/SplitActivityWizard.h`, `src/Gui/SplitActivityWorkflow.h`, and
  `src/Planning/PlanBundle.cpp`
- Impact: Save/Discard preflight, Save-and-Convert, Save-on-Exit, the first delete
  confirmation, and Split wizard pages all retained raw `RideItem*` values across
  nested event loops. A queued removal could cause use-after-free, save a dangling
  item, or continue a split using a different/no-longer-cached source. Linked Save
  could also dereference a peer whose ride failed to load.
- Test-first evidence: The first guarded-preflight test did not compile because
  no lifetime API existed; after adding the declaration it rejected a destroyed
  item. Two additional RED dialog tests showed Save-and-Convert accepted a
  destroyed item and Save-on-Exit invoked its virtual save with a dangling
  pointer. Core linked and batch callback tests separately crashed or failed
  before their guards were added. Follow-up RED rows showed that Save-on-Exit
  accepted newly dirty and re-dirtied activities, Discard accepted a failed
  reload and could outlive an activity destroyed by `close()`, and candidate
  setters could destroy the current or replacement row. Split accepted an
  in-place RideFile revision change, and a cache callback could continue after
  one of its owners was destroyed.
- Resolution: Modal inputs are captured as `QPointer` before each event loop and
  resolved again before Save or Discard. Save dialogs and Save All retain guarded
  snapshots, reject detached or identity-mutated rows, and do not dereference a
  dialog after a nested save callback destroys it. A save that durably commits
  on the same path before its ride disappears remains successful, while a
  path-changing save rolls back its new target if ownership disappears. The
  Save-on-Exit worklist is reconciled after every callback, including newly dirty
  and re-dirtied rows, and its abandon path marks the same complete set. Discard
  requires a successful guarded reload. Candidate saves revalidate every owner
  after save and setter callbacks. Delete guards both Context and item before its
  first confirmation and adopts only a valid current cache identity after Save
  renames the activity.
  Split owns a guarded source, checks cache identity after confirmation and before
  post-publish eviction, verifies both RideFile identity and revision at each
  boundary, and every page/plot handles a missing ride. Filename prediction now
  fails closed for null or unloadable rides. Repeat Plan pages resolve their
  cache through the guarded wizard, PlanBundleReader tracks a destroyed Context,
  and cache callback continuations use one owner guard.
- Verification: The directly testable save/preflight paths pass in the 115-case
  atomic-save program, linked and batch callbacks pass in the 231-case removal
  program, split paths pass their 28- and 33-case programs, and the deletion,
  save, Repeat Plan, plan-reader, and callback contracts pass 28, 15, 27, 7, and
  3 cases. These focused suites pass under ASan/UBSan and under TSAN; all except
  the two Qt model rows in `TEST-007` are also strict-LSan-clean. The complete
  production application and 99-target matrix pass. Real MainWindow, Calendar,
  Batch, Repeat Plan, PlanBundle, and Split dialog interaction remains tracked
  by `TEST-005`.

### GUI-010: Other Calendar modal workflows retain mutable owner state

- Status: FIXED
- Code: `src/Charts/CalendarWindow.cpp`,
  `src/Charts/CalendarSeasonWorkflow.h`, `src/Core/Season.h`,
  `src/Gui/AnalysisSidebar.cpp`, `src/Gui/FilterSimilarDialog.cpp`,
  `src/Gui/ManualActivityWizard.cpp`, `src/Gui/ModalWorkflowGuard.h`,
  `src/Gui/PlanWizards.cpp`, and `src/Train/ErgFile.h`
- Impact: Manual Activity, Import/Export Plan, Filter Similar, activity
  linking, and season event/phase dialogs run nested event loops while retaining
  raw `Context`, `AthleteTab`, `RideItem`, `Season`, `Phase`, or `SeasonEvent`
  state. Closing or replacing the athlete from a queued callback can invalidate
  those objects. The add/import paths also restore `noSwitch` to `false`
  unconditionally, which can clear an outer navigation guard or modify a newly
  selected tab.
- Test-first evidence: The first focused build failed because the complete-owner,
  active-tab, exact-affected-set, guarded-continuation, mutation-rejection,
  committed-boundary, workout-lifetime, and current-season contracts did not
  exist. A later regression reproduced the remaining pre-`exec()` gap: losing
  an owner before installing the rejection hook returned `Accepted` instead of
  `Rejected` after the nested loop began.
- Resolution: A shared modal guard now tracks every QObject owner and validates
  the active MainWindow/AthleteTab topology before commit. Dialogs reject on
  owner destruction or source mutation, including owner loss immediately before
  `exec()`. Navigation and temporary workout overrides use owner-aware leases
  that restore only state they still own and never restore an expired workout.
  Calendar operations snapshot and recheck the exact affected object set,
  re-resolve stable activity identities after save-driven renames, preserve
  committed outcomes as non-retryable, and guard continuations after nested
  warnings. Season editing uses detached values, stable event UUIDs, unique
  record resolution, unchanged-season snapshots, and the still-current season
  identity. Link persistence saves every surviving affected item and reports an
  incomplete save instead of silently skipping it.
- Verification: All 37 modal-workflow cases pass normally and under strict
  ASan/UBSan/LSan. The 9-case season parser and 27-case Repeat Plan contracts
  pass both configurations. The complete production application links, and an
  isolated 15-second offscreen event-loop smoke test with a disposable home
  directory does not exit or crash. The expanded Windows, macOS, and Linux
  workflow is clean under `actionlint` 1.7.12. Hosted run `30962466123` passes
  all three jobs, and standard build run `30962466139` builds, tests, and
  uploads the macOS package successfully.

### GUI-011: Repeat Plan treated committed outcomes as retryable failures

- Status: FIXED
- Code: `src/Gui/PlanWizards.cpp`, `src/Gui/PlanWizards.h`, and
  `src/Charts/CalendarWindow.cpp`
- Impact: The replacement journal could commit the complete new plan and then
  report owner loss, a post-commit processor warning, or a cache notification
  count mismatch. The wizard classified all of those as failure and remained
  open, inviting a second attempt after storage had already changed. A reentrant
  acceptance callback could also enter completion twice, and duplicate deletion
  pointers inflated the expected removal count.
- Test-first evidence: Disposition rows distinguish uncommitted owner loss and
  backend failure from committed owner loss, committed warnings, and committed
  count mismatches. A separate regression re-enters completion and requires the
  second call to be rejected; a legacy-overload row preserves its original enum
  values and owner-loss behavior.
- Resolution: Replacement disposition now includes the durable `committed`
  boundary. Uncommitted outcomes keep the wizard open, while committed outcomes
  are accepted exactly once and any post-commit issues are shown as warnings.
  Expected removals are deduplicated, one execution guard blocks reentrancy, and
  Calendar revalidates all owners after the modal loop before refreshing.
- Verification: The complete 27-case Repeat Plan contract passes strict
  ASan/UBSan/LSan. The production application build covers the real wizard and
  Calendar translation units; visible widget interaction remains in `TEST-005`.

### THREAD-022: Credential shutdown can destroy live settings state

- Status: FIXED
- Code: `src/Core/Settings.cpp`, `src/Core/Settings.h`,
  `src/Core/CredentialStoreQtKeychain.cpp`,
  `src/Cloud/StravaSettingsCommit.cpp`, and `src/Core/main.cpp`, plus
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: Credential operations release the `GSettings` mutex while retaining
  pointers to settings and backend state. Reconfiguration or application
  shutdown could delete those objects after the worker's 100 ms join expired,
  allowing a stalled keychain callback to resume through freed settings,
  storage, or mutex memory.
- Test-first evidence: `credentialBackendBlocksSettingsReconfigurationUntilRelease`
  blocks a backend after the settings mutex is released, proves that bounded
  reconfiguration fails without destroying state, and then proves that an
  unbounded reconfiguration completes only after backend release.
  `credentialWorkerShutdownAbandonsQueuedOperations` holds the active operation
  beyond the 100 ms worker join, concurrently destroys its `GSettings`, and
  requires destruction to remain blocked until release while queued work and
  stale completion callbacks remain suppressed.
- Resolution: Every settings-backed credential call increments a suspension
  lease before releasing the settings mutex and clears it only after the call
  stack no longer retains settings or backend objects. Reconfiguration and the
  `GSettings` destructor wait for all foreign leases. Credential-worker
  generations suppress callbacks after shutdown, and every production shutdown
  path uses `_Exit` before settings teardown if the bounded worker join does not
  complete.
- Verification: Implementation commit `906b5e3`. The complete 15-case
  `THREAD-019..022` matrix passes ThreadSanitizer with no race reports. The full
  credentialSettings program passes 443 cases under strict
  ASan/UBSan/LSan, with zero failures and seven platform skips.

### GUI-013: Linux startup dereferences an unavailable OpenGL dispatch table

- Status: FIXED
- Code: `src/Gui/MainWindow.cpp`, `src/Gui/OpenGLVersionProbe.cpp`,
  `src/Gui/OpenGLVersionProbe.h`, and
  `unittests/Gui/openGLVersionProbe/`
- Impact: Linux startup ignored failures from OpenGL context creation and
  `makeCurrent()`, then dispatched `glGetString()` through that invalid
  context. Systems without a usable OpenGL context, including the packaged Qt
  offscreen smoke environment, could crash before the main window became
  ready.
- Test-first evidence: The packaged offscreen smoke reproduced a segmentation
  fault in `MainWindow` after Qt reported a non-current OpenGL context. The
  focused invalid-context regression was added before the checked probe existed
  and failed to build because no safe failure-path API was available.
- Resolution: OpenGL version discovery now validates context creation, surface
  creation, `makeCurrent()`, and the function table before dispatch. Every
  successful current-context path calls `doneCurrent()`, while any unavailable
  OpenGL capability returns an empty version and allows startup to continue.
- Verification: All five focused cases pass normally and under strict
  ASan/UBSan, covering null surfaces, an invalid context, and automatic cleanup
  with no usable OpenGL context. Complete packaged offscreen verification is
  required before release promotion.

## Medium

### DATA-023: A missing workout setting becomes a literal `0` path

- Status: FIXED
- Code: `src/Planning/PlanBundle.cpp` and
  `unittests/Core/rideCacheRemoval/testRideCacheRemoval.cpp`
- Impact: `GSettings::value()` defaults its fallback `QVariant` to integer zero.
  Plan import recovery omitted an explicit fallback, converted that zero to the
  string `"0"`, and skipped its intended athlete-library default. A fresh
  profile therefore reported that the workout library was unavailable before
  the main window had initialized the setting.
- Test-first evidence: `planBundleRecoveryUsesDefaultWorkoutRoot()` leaves the
  setting absent, supplies an existing athlete-library parent, and first failed
  with `The workout library is unavailable`.
- Resolution: The recovery path now requests an explicit empty `QString` from
  settings. The existing empty-value branch then resolves and canonicalizes the
  athlete-library parent as intended.
- Verification: The focused recovery regression passes against the production
  `PlanBundle::reconcilePendingImport()` implementation. The complete
  RideCache removal, mutation, and recovery program passes 407 tests with zero
  failures and one expected platform skip.

### BUILD-037: Offscreen startup runs a desktop WebEngine workaround

- Status: FIXED
- Code: `src/Gui/MainWindow.cpp`, `src/Gui/GuiStartupPolicy.cpp`,
  `src/Gui/GuiStartupPolicy.h`, and `unittests/Gui/guiStartupPolicy/`
- Impact: Main-window construction always created and immediately destroyed a
  temporary `QWebEngineView` to avoid flicker on real desktop windows. The
  workaround has no purpose on Qt's offscreen platform, where it starts an
  additional WebEngine rendering lifetime that the package smoke must tear
  down despite having no visible surface.
- Test-first evidence: Retained offscreen startup first reproduced entry into
  Qt Quick and WebEngine teardown after the ready marker. The platform-policy
  regression was then added before its production API and failed to build
  because no guarded policy existed.
- Resolution: The desktop primer is preserved for normal platform plugins but
  is skipped case-insensitively for `offscreen`. The package smoke can therefore
  validate the main window without creating a rendering workaround that cannot
  affect an invisible surface.
- Verification: The focused policy covers lowercase and mixed-case offscreen
  names plus xcb, Wayland, Windows, and Cocoa desktop controls, with all eight
  rows passing in normal and ASan/UBSan builds. A production constructor trace
  confirms that the primer is absent offscreen; a separate map-view shutdown
  defect still blocks the complete package smoke.

### MEM-028: OAuth nested messages can outlive their dialog

- Status: FIXED
- Code: `src/Cloud/OAuthDialog.cpp`, `src/Cloud/OAuthDialog.h`, and
  `src/Cloud/OAuthDialogMessageGuard.cpp`
- Impact: OAuth failure paths open nested message-box event loops and then
  continue through the raw dialog pointer. Closing or deleting the OAuth dialog
  reentrantly can therefore cause a use-after-free when the message returns.
- Test-first evidence: The failure-message regression destroys the dialog from
  network-failure, malformed-JSON, and invalid-Strava-response paths. Success
  and constructor-message rows separately delete the live dialog or message
  while callbacks remain queued.
- Resolution: Commit `906b5e3` routes OAuth messages through a nonblocking
  guard. It tracks dialogs and messages with `QPointer`, binds completion to a
  live Qt context, and rejects or accepts only while the target still exists.
- Verification: The implementation's OAuth package passed under strict
  ASan/UBSan/LSan. At integration revision `daae182`, the current production
  helper and OAuth sources pass all 68 focused cases, including every dialog
  destruction row.

### DUR-016: Cross-process Strava recovery can use stale revocation state

- Status: FIXED
- Code: `src/Cloud/StravaCredentialDurability.cpp`,
  `src/Cloud/StravaCredentialPublisher.cpp`,
  `src/Cloud/StravaTokenRefresh.cpp`, and
  `src/Cloud/StravaAccountRemoval.cpp`
- Impact: Durable recovery records are coordinated with process-local state.
  Another GoldenCheetah process can rotate, revoke, or replace a grant before
  recovery resumes, allowing stale recovery state to overwrite or misclassify
  the newer authorization.
- Test-first evidence: `independentProcessesSerializeAndFenceGenerations`
  starts refresh, removal, and OAuth child processes against one disposable
  account. It requires generations 1, 2, and 3 to serialize, each child to see
  its predecessor, and the final OAuth grant to win. Process-death, coherent
  revision-read, and lock/journal parent-swap rows cover recovery boundaries.
- Resolution: Commit `906b5e3` adds an account-derived anchored interprocess
  lease, refreshes storage metadata after acquiring it, and binds every journal
  transition to a transaction ID and generation. Credential and authorization
  revisions are checked around coherent snapshots and before publication, so a
  stale process cannot publish through a newer generation.
- Verification: The implementation's 26-case durability suite passed normally
  and under strict ASan/UBSan/LSan. The expanded suite, built exactly from
  integration revision `daae182`, passes all 28 cases.

### THREAD-019: QtKeychain caller deadlines do not bound backend completion

- Status: FIXED
- Code: `src/Core/CredentialStoreQtKeychain.cpp`,
  `src/Core/CredentialStoreQtKeychain.h`, and
  `src/Cloud/StravaSettingsCommit.cpp`
- Impact: A caller can time out while the QtKeychain job remains live and may
  later mutate the vault. The apparent deadline therefore does not bound the
  operation or establish whether a credential write committed.
- Test-first evidence: The timeout regressions stall read, write, and removal
  jobs beyond both native and caller deadlines, attempt competing mutations,
  and then finish or destroy the original job. They require a tracked pending
  or indeterminate result, retained process and filesystem serialization, and
  deterministic retry only after terminal reconciliation.
- Resolution: A timed-out native mutation retains its unique job gate and
  durable backend marker until the job finishes or is destroyed. Callers see
  `Pending` or `Indeterminate`, never success or an empty credential, and the
  Strava durability transaction remains authoritative for late completion.
  Generation checks prevent an old completion from releasing or publishing
  through a newer owner.
- Verification: Implementation commit `906b5e3`. The complete 15-case
  `THREAD-019..022` matrix passes ThreadSanitizer with no race reports. The full
  credentialSettings program passes 443 cases under strict
  ASan/UBSan/LSan, with zero failures and seven platform skips.

### THREAD-020: Credential suspension skips valid settings operations

- Status: FIXED
- Code: `src/Core/Settings.cpp`, `src/Core/Settings.h`, and
  `src/Core/CredentialSettings.cpp`
- Impact: A per-instance credential suspension counter makes unrelated settings
  calls fail or return early. In particular, opening or creating an athlete
  during a blocked credential request can silently skip same-instance settings
  initialization with no retry.
- Test-first evidence: Block a credential read, invoke
  `initializeQSettingsAthlete()` on the same `GSettings` instance, release the
  read, and require initialization exactly once. Also require unrelated
  settings access to wait or proceed rather than report a false failure.
- Resolution: Production structural operations wait for credential-backend
  suspensions instead of skipping initialization. The wait temporarily releases
  the recursive settings mutex and, on the application thread, processes the
  restricted event path needed for native keychain completion. A suspension
  owned by the current reentrant stack fails closed rather than deadlocking;
  the caller retries that deferred structural operation after the owning lease
  unwinds. A call from another thread waits and completes exactly once.
- Verification: Implementation commit `906b5e3` includes the same-instance
  initialization and reentrant application-thread regressions. The complete
  15-case `THREAD-019..022` matrix passes ThreadSanitizer, and the full
  credentialSettings program passes 443 cases under strict ASan/UBSan/LSan.

### THREAD-021: Bounded credential-worker shutdown can leave work alive

- Status: FIXED
- Code: `src/Cloud/StravaSettingsCommit.cpp`,
  `src/Cloud/StravaSettingsCommit.h`, and `src/Core/main.cpp`
- Impact: Shutdown waits 100 ms and can return while a keychain or settings
  operation still runs. The process then has neither a reliable completion
  result nor a bounded guarantee that all credential work has stopped.
- Test-first evidence: Hold a worker operation beyond
  100 ms, initiate shutdown, then release it and require deterministic joining
  or durable handoff with no live worker at process teardown.
- Resolution: Shutdown atomically stops submissions, abandons queued operations,
  interrupts the active worker, and invalidates its callback generation. A
  cooperative operation joins and its stopped generation is reclaimed. If a
  native backend remains wedged past 100 ms, production does not tear down any
  referenced application state: both startup termination and normal shutdown
  take an explicit `_Exit` fail-stop path. Durable credential journals retain
  any mutation whose outcome is not definitive.
- Verification: Implementation commit `906b5e3`. The shutdown child regression
  proves bounded return, callback suppression, blocked settings destruction,
  eventual join after release, and no queued execution. It passes in the
  15-case ThreadSanitizer matrix and the full 443-case ASan/UBSan/LSan run.

### DUR-017: Replaced split-journal payloads had ambiguous cleanup ownership

- Status: FIXED
- Code: `src/FileIO/AnchoredFileSystem.cpp`,
  `src/FileIO/AnchoredFileSystem.h`, and `src/Gui/SplitActivitySave.cpp`
- Impact: Cleanup could encounter a journal payload whose pathname had been
  replaced after validation. Without identity, generation, size, and digest
  continuity, deleting it could destroy foreign data while accepting it could
  erase the evidence needed for recovery.
- Test-first evidence / required regression: Replace the source, backup, or
  output cleanup payload, and separately alter output size. Recovery must retain
  the replacement and journal and report a payload-specific error.
- Resolution: Cleanup derives expected payload evidence from the pinned
  manifest, repins each observed journal file, and removes a payload only when
  identity, durable generation, size, and SHA-256 all match. Ambiguous files
  are preserved with explicit diagnostics.
- Verification: Commit `220a96f`; the split suite passes 104/104 normally,
  under ASan/UBSan, and under ThreadSanitizer. The atomic suite passes 331/331,
  the full application links, and the isolated offscreen smoke reaches timeout.

### DUR-018: Split recovery lacked durable-generation evidence

- Status: FIXED
- Code: `src/FileIO/AnchoredFileSystem.cpp`,
  `src/FileIO/AnchoredFileSystem.h`, and `src/Gui/SplitActivitySave.cpp`
- Impact: Native identity, size, and digest alone do not prove that a pathname
  still names the same durable file generation after replacement and reuse.
  Forward recovery could otherwise publish or remove an unproven artifact.
- Test-first evidence / required regression: Use a filesystem fixture that
  cannot provide generation evidence and replace or reuse an output identity;
  recovery must fail closed, preserve production data, and retain its journal.
- Resolution: Pinned files expose platform-backed durable-generation evidence.
  Split manifests and cleanup records require and validate that evidence at
  every publication, recovery, and retirement boundary.
- Verification: Commit `220a96f`; the split suite passes 104/104 normally,
  under ASan/UBSan, and under ThreadSanitizer. The atomic suite passes 331/331,
  the full application links, and the isolated offscreen smoke reaches timeout.

### DUR-019: Split recovery limits did not charge actual reads

- Status: FIXED
- Code: `src/FileIO/AnchoredFileSystem.cpp`,
  `src/FileIO/AnchoredFileSystem.h`, and `src/Gui/SplitActivitySave.cpp`
- Impact: Recovery limits based only on declared metadata can be bypassed by
  actual payload reads, while a deadline checked only between files permits one
  large read or digest operation to monopolize startup.
- Test-first evidence / required regression: Recover oversized and aggregate
  payload sets under a shared byte budget, expire the deadline in the middle of
  a payload read, and require bounded failure followed by successful resumable
  recovery.
- Resolution: One recovery budget is shared across journals and phases. Every
  bounded read and digest callback charges actual bytes in chunks of at most
  one MiB and checks the common operation limit and deadline.
- Verification: Commit `220a96f`; the split suite passes 104/104 normally,
  under ASan/UBSan, and under ThreadSanitizer. The atomic suite passes 331/331,
  the full application links, and the isolated offscreen smoke reaches timeout.

### PERF-013: Split-manifest validation can become quadratic

- Status: FIXED
- Code: `src/Gui/SplitActivitySave.cpp`
- Impact: Repeated linear searches and path comparisons across a maximum-size
  hostile manifest can make startup recovery quadratic and hold application
  initialization for an excessive time.
- Test-first evidence: Validate maximum-sized manifests
  with unique and adversarially colliding path sets, measure comparison or
  lookup counts, and require linear or near-linear growth.
- Resolution: Commit `220a96f` normalizes each path once, sorts the normalized
  keys, and checks only adjacent paths for equality or ancestry. Identity and
  cleanup-name uniqueness use keyed sets and maps while retaining anchored
  filesystem validation.
- Verification: The maximum-size regression validates 1000 outputs with a
  bounded path-validation step count, and hostile-manifest coverage rejects a
  source/output collision. Both are included in the split suite that passes
  104/104 normally, under ASan/UBSan, and under ThreadSanitizer.

### DUR-020: Multi-target overwrite can publish before all predecessors validate

- Status: FIXED
- Code: `src/Planning/PlanBundleImportJournal.cpp` and
  `src/Train/StravaRoutesDownload.cpp`
- Impact: A batched overwrite can publish an early target before discovering
  that a later predecessor was replaced or became unsafe. Failure then exposes
  a partially updated route set even though the transaction was rejected.
- Test-first evidence: `standaloneMultiTargetPreflightIsMutationFree()` replaces
  the second predecessor after the durable decision and before publication. Its
  create and overwrite rows require the first target to remain absent or retain
  its original bytes while the foreign second generation is preserved.
- Resolution: Commit `47b70c3` locks every target and captures each current
  file through the anchored directory before publication. It validates the
  complete batch against the persisted predecessor size, SHA-256, and native
  generation fingerprint before preparing or moving the first output.
- Verification: The PlanBundleImport regression is part of the tested route
  integration. The 48-case production route suite and complete build passed in
  workflow `31088424022`; the relevant production and test files are unchanged
  between `47b70c3` and this audit reconciliation.

### DUR-021: Staged route publication is not bound to its original pin

- Status: FIXED
- Code: `src/Train/StravaRoutesDownloadPipeline.cpp`,
  `src/Train/StravaRoutesDownloadPipeline.h`, and
  `src/Train/StravaRoutesDownload.cpp`
- Impact: Route preparation and later publication trust a staging pathname
  across phases. Replacement at that pathname can cause the importer to parse
  or publish bytes other than the authenticated download.
- Test-first evidence: `pinnedRouteBytesSurviveStagingParentSwap()` proves a
  validated read continues from the original handle during a directory swap,
  while `stagedRouteRejectsSameSizedPrevalidationReplacement()` substitutes a
  same-sized foreign generation and requires rejection before the parser runs.
- Resolution: Commit `47b70c3` carries an anchored `StagedRoutePin`, byte count,
  and SHA-256 with each staged route. Preparation verifies the pathname still
  names that pin before and after streaming, hashes the pinned bytes again, and
  passes only the authenticated in-memory bytes to the parser and durable
  publication journal. Later phases no longer reopen the staging pathname.
- Verification: The pin-swap regressions and real production composition are in
  the 48-case route suite that passed in workflow `31088424022`; the relevant
  implementation and tests are unchanged at this reconciliation revision.

### GUI-012: Abort is ignored during route import preparation

- Status: FIXED
- Code: `src/Train/StravaRoutesDownload.cpp` and
  `src/Train/StravaRoutesDownloadPipeline.cpp`, plus
  `src/FileIO/GpxParser.cpp`
- Impact: After downloads complete, route parsing, hashing, and preparation can
  run for long enough that Close or Abort appears ineffective. The suffix can
  continue consuming CPU and may be imported despite the user's request.
- Test-first evidence: `delayedPinnedReadCancellationKeepsGuiResponsive()` and
  `delayedGpxParserCancellationKeepsGuiResponsive()` stall both expensive
  preparation phases and require prompt worker exit while GUI heartbeats
  continue. Prefix, queued-stage, and actual dialog-slot regressions cover
  between-route cancellation, Abort, Close, and window close.
- Resolution: Commit `47b70c3` moves preparation to an owned worker, polls its
  lock-free cancellation check between routes and inside bounded file, XML,
  interpolation, and parser work, and generation-fences every queued handoff.
  Abort discards uncommitted preparation; a decision that became durable is
  retained for recovery instead of being misreported or rolled back.
- Verification: The 48-case route suite passed normally and under strict
  ASan/UBSan/LSan during the original fix. Its unchanged production composition
  also passed the complete build in workflow `31088424022`.

### TEST-008: Route-import composition test substitutes a fake parser

- Status: FIXED
- Code: `unittests/Train/stravaRoutesDownloadPipeline/`,
  `src/Train/ErgFile.cpp`, `src/Train/GpxParser.cpp`, and
  `src/Train/TrainDB.cpp`
- Impact: The byte-backed route test replaces production parsing with a fake,
  so it can pass while real GPX-to-`ErgFile` composition, metadata transfer, or
  TrainDB import wiring is broken.
- Test-first evidence: `byteBackedErgFileSurvivesParserTemporaryCleanup()` feeds
  real GPX bytes through `ErgFile::fromGpxContentBytes()`, verifies route points,
  altitude and location after parser cleanup, imports the result into a
  disposable `TrainDB`, and checks the persisted workout row and type.
- Resolution: Commit `47b70c3` links the production `GpxParser`, `ErgFile`,
  byte-backed adapter, `TrainDB`, and journal into the focused suite. Its small
  composition stub supplies only isolated settings and power-zone dependencies;
  it does not replace route parsing, workout construction, or persistence.
- Verification: Real multi-route commit, rollback, cancellation, restart
  recovery, and dialog composition are included in the 48-case suite. The
  complete build passed in workflow `31088424022`, and these sources are
  unchanged at this audit reconciliation.

### DUR-022: Unavailable vault reads look empty during local Strava removal

- Status: FIXED
- Code: `src/Cloud/StravaCredentialPublisher.cpp`,
  `src/Cloud/StravaCredentialDurability.cpp`, and
  `src/Cloud/StravaAccountRemoval.cpp`
- Impact: Backend read failure is converted to empty tokens and then marked
  readable. Local-only disconnect can report success and persist `revoked`
  while real credentials remain in an unavailable vault.
- Test-first evidence: `unavailableVaultReadCannotReportLocalDisconnect` makes
  local-only token reads unavailable and requires no success, disconnect,
  deletion, or `revoked` state. After backend recovery, the same regression
  requires retry to remove both original tokens and complete revocation.
- Resolution: Commit `906b5e3` preserves checked credential-read status through
  the publisher and removal coordinator. A local removal proceeds only when
  both token reads are authoritative; `Unavailable` fails closed without
  converting unknown credentials into an empty pair or publishing success.
- Verification: All 21 account-removal cases passed under strict
  ASan/UBSan/LSan with the implementation and pass again against integration
  revision `daae182`.

### DUR-023: Revocation becomes uncertain before any remote dispatch

- Status: FIXED
- Code: `src/Cloud/StravaAccountRemoval.cpp`,
  `src/Cloud/StravaCredentialDurability.cpp`, and
  `src/Cloud/StravaRevocationClient.cpp`
- Impact: Removal enters `CommitUnknown` before distinguishing local-only work
  or proving that a remote request was dispatched. A crash during local cleanup
  or request-construction failure can therefore leave authorization permanently
  uncertain even though no remote side effect was possible.
- Test-first evidence: Local and confirmed-remote crash rows restart before the
  first credential deletion. `requestCreationFailureRestoresRetryableAuthorization`
  requires a pre-dispatch failure to restore the previous authorization, while
  the dispatched-request rows retain fail-closed uncertainty.
- Resolution: Commit `906b5e3` separates local cleanup from the remote boundary
  and carries an explicit `mayHaveBeenDispatched` result from request creation.
  A failure before dispatch retires the uncertain transition and restores the
  prior retryable state; once dispatch may have occurred, recovery remains
  blocked until the durable remote outcome and local-commit phase reconcile.
- Verification: The implementation's durability and account-removal programs
  passed under strict ASan/UBSan/LSan. At integration revision `daae182`, their
  expanded focused suites pass 28/28 and 21/21 cases respectively.

### DUR-024: macOS root aliases made anchored persistence unusable

- Status: FIXED
- Code: `src/FileIO/AnchoredFileSystem.cpp` and
  `unittests/FileIO/anchoredFilesystem/testAnchoredFilesystem.cpp`
- Impact: The anchored-directory walker rejected every symbolic path component
  with `O_NOFOLLOW`. macOS exposes the system-owned `/var` directory as a link
  to `/private/var`, so ordinary temporary paths below `/var/folders` failed
  before their actual directory could be opened. This broke atomic persistence
  for otherwise safe paths and cascaded into 192 credential-settings failures
  on the native macOS runner.
- Test-first evidence: A focused regression first reproduced the native `/var`
  failure on macOS and, in a root-owned Linux fixture, failed with `Not a
  directory`. A separate regression requires a user-controlled directory alias
  to remain rejected.
- Resolution: The Unix walker may resolve only its first root-level component,
  and only when that component is a root-owned symbolic link. It reads the
  target relative to the already opened root descriptor, verifies device,
  inode, type, and owner again after the read, then restarts traversal from the
  cleaned absolute target. Every later component remains protected by strict
  descriptor-relative `O_NOFOLLOW` traversal, so user-controlled aliases and
  alias chains are still rejected.
- Verification: The focused Linux suite passes 89 cases with 14 platform
  skips, and the complete credential suite passes 427 cases with seven
  platform skips. GitHub Actions run 31105481269 builds and passes the anchored
  filesystem test on macOS 15 and both filesystem suites on Windows 2025.

### DUR-025: Windows name tunneling misclassified successful publications

- Status: FIXED
- Code: `src/FileIO/AnchoredFileSystem.cpp` and
  `unittests/FileIO/anchoredFilesystem/testAnchoredFilesystem.cpp`
- Impact: NTFS can preserve the creation time associated with a recently
  vacated filename. `ReplaceFileW` and rename therefore changed the pinned
  file's creation-time component even though its volume, native file ID, byte
  extent, modification time, and contents still identified the published
  source. The anchored helpers reported a partial failure after the mutation
  had already succeeded, leaving callers in unnecessary recovery state.
- Test-first evidence: The Windows regressions assign distinct creation times
  to replacement generations and move copied output into a just-vacated name.
  The original implementation failed the tunneled move in workflow
  `31574659831`; ordinary copied-output moves provide the non-tunneled control.
- Resolution: Commits `ee425df` and `7962023` verify post-publication identity
  with the stable Windows volume and file ID plus extent, modification time,
  and SHA-256. They accept only the documented creation-time inheritance and
  refresh the pinned identity and durable-generation evidence after the native
  operation; unrelated substitutions still fail closed.
- Verification: Focused workflow `31574854649` passed the Windows and macOS
  regressions. Full native workflow `31579686400` subsequently passed Linux,
  macOS, and Windows after portable fixture and responsiveness stabilization.

### BUILD-014: Vendored Python metadata can claim false file ownership

- Status: OPEN
- Code: `src/Resources/linux/generate-runtime-provenance.py`,
  `src/Resources/linux/normalize-embedded-python.py`, and
  `src/Resources/linux/MakeAppImageQt6.sh`
- Impact: A vendored `.dist-info` directory can be treated as proof that nearby
  Python files belong to that distribution even when authenticated package
  metadata does not claim them. The AppImage SBOM and provenance can therefore
  attribute injected or unowned code to a legitimate dependency.
- Test-first evidence / required regression: Package a fixture with legitimate
  distribution metadata plus an unclaimed module and require provenance
  generation to reject or explicitly mark the module unowned.
- Fix direction: Derive ownership from authenticated wheel or installed-package
  records with normalized exact paths and hashes; fail closed on overlaps,
  missing claims, or conflicting metadata.
- Verification: Confirmed by build review. Candidate implementation and tests
  are confined to unintegrated agent work.

### BUILD-015: Bundled Python runtime lacks authenticated provenance

- Status: OPEN
- Code: `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/generate-runtime-provenance.py`, and
  `src/Resources/linux/generate-appimage-sbom.py`
- Impact: Hashing the final embedded interpreter records what was packaged but
  does not authenticate where the Python runtime and standard library came
  from. A substituted runtime can receive internally consistent provenance.
- Test-first evidence / required regression: Substitute runtime bytes while
  retaining plausible local metadata and require packaging to fail unless each
  source artifact is covered by the locked manifest and verified digest.
- Fix direction: Bind every runtime file to a hash-locked source archive or
  package record and carry that authenticated source identity into the embedded
  provenance and SBOM.
- Verification: Confirmed by build review. Provenance tooling exists only in
  unintegrated work and has not yet passed the integrated release gate.

### BUILD-016: Runtime provenance uses incompatible path ordering

- Status: OPEN
- Code: `src/Resources/linux/generate-runtime-provenance.py` and
  `src/Resources/linux/generate-appimage-sbom.py`
- Impact: Sorting or comparing mixed filesystem `Path` and POSIX-path values can
  raise a type error or produce platform-dependent ordering. Packaging can fail
  or emit nondeterministically ordered provenance from equivalent inputs.
- Test-first evidence / required regression: Feed mixed native and normalized
  POSIX path objects, including non-ASCII and case-sensitive names, and require
  stable byte-identical ordering without cross-type comparison.
- Fix direction: Normalize to one canonical relative POSIX string at the trust
  boundary and sort only by that explicit key.
- Verification: Confirmed by build review. The normalization change is not
  integrated or release-verified.

### BUILD-017: Release archives are extracted without a safe boundary

- Status: OPEN
- Code: `appveyor/safe-extract.py`, `appveyor/linux/install.sh`,
  `appveyor/macos/install.sh`, `appveyor/windows/install.ps1`, and
  `appveyor/linux/after_build.sh`
- Impact: Dependency archives can contain traversal paths, absolute paths,
  links, device names, or collisions that escape or corrupt the build tree.
  AppVeyor Linux also has a direct `tar` path that bypasses any shared safety
  checks.
- Test-first evidence / required regression: Exercise malicious ZIP and TAR
  members, links, path aliases, collisions, and the AppVeyor direct-tar call;
  require rejection before any destination mutation.
- Fix direction: Route every platform and phase through one fail-closed staged
  extractor that validates the complete member set before publishing files.
- Verification: Confirmed by build review. The shared extractor and its tests
  remain unintegrated, including the direct-tar bypass correction.

### BUILD-018: Qt installation trusts unlocked layout and updater behavior

- Status: OPEN
- Code: `.devcontainer/install-verified-qt.py`,
  `.devcontainer/aqt-requirements.lock`,
  `.devcontainer/qt-6.8.3-linux-gcc64.lock`, and
  `.devcontainer/Dockerfile`
- Impact: A pinned top-level Qt version is insufficient when the downloader,
  module layout, metadata, or updater can change independently. The same source
  can install different toolchains or execute different installer code.
- Test-first evidence / required regression: Alter updater bytes, archive
  membership, module layout, or an archive digest and require installation to
  fail before extraction; verify the complete expected layout after install.
- Fix direction: Hash-lock the installer and every Qt archive, validate exact
  archive membership and destination layout, and disable implicit updates or
  network fallback.
- Verification: Confirmed by build review. Locks and installer validation are
  present only in unintegrated agent work.

### BUILD-019: CI dependencies are mutable and their gate expects tags

- Status: OPEN
- Code: `.github/workflows/ci.yml`,
  `.github/workflows/ridecache-removal-native.yml`,
  `.github/workflows/windows-durable-filesystem.yml`, and
  `unittests/Build/ciTestRunner/testCiTestRunner.py`
- Impact: Tag-based GitHub Actions can change without a source commit, while a
  stale self-test that requires `upload-artifact@v7` rejects the safer
  full-commit SHA form. CI is either mutable or fails after being pinned.
- Test-first evidence / required regression: Require every external action to
  use a full approved commit SHA and make the self-test reject tags while
  accepting the exact pinned revision.
- Fix direction: Pin actions by reviewed commit SHA, record the human-readable
  release in comments or lock metadata, and make tests parse semantic action
  identity separately from the immutable revision.
- Verification: Confirmed by build and failed-workflow review. Corrected pins
  and expectations are not yet integrated or rerun in platform CI.

### BUILD-020: Runtime provenance override is fail-open and forgeable

- Status: OPEN
- Code: `src/Resources/linux/AppImagePackagingSupport.sh` and
  `src/Resources/linux/generate-runtime-provenance.py`
- Impact: A package-index environment override can supply arbitrary ownership
  claims and let production packaging generate apparently valid provenance for
  unauthenticated files.
- Test-first evidence / required regression: Set the override in normal
  packaging and require failure. In explicit test mode, alter the fixture path
  or bytes after authorization and require its bound digest check to fail.
- Fix direction: Remove the production override. Permit fixtures only behind an
  explicit test mode and bind the canonical fixture path and SHA-256 before use.
- Verification: Confirmed by build review. Candidate restrictions and tests are
  still in unintegrated agent work.

### BUILD-021: Native releases lack OAuth credential gates

- Status: OPEN
- Code: `appveyor/macos/after_build.sh`,
  `appveyor/windows/after_build.ps1`, `appveyor/check-unconfigured-oauth.py`,
  and `src/Core/Secrets.h`
- Impact: AppImage packaging validates configured OAuth credentials, but macOS
  and Windows artifacts can ship placeholder, absent, malformed, or unintended
  credentials while still being presented as production builds.
- Test-first evidence / required regression: Build configured and unconfigured
  native fixtures and require each release job to reject placeholders, missing
  support, malformed status output, and credentials not intended for release.
- Fix direction: Apply one executable-backed OAuth configuration contract to
  Linux, macOS, and Windows immediately before signing or publication.
- Verification: Confirmed by release review. Native gates and platform tests are
  unintegrated and have not run in their final CI environments.

### BUILD-022: CI test inventory is incomplete and manually maintained

- Status: OPEN
- Code: `.github/scripts/run-tests.py`, `unittests/unittests.pro`,
  `unittests/ci-required-tests.txt`, and
  `unittests/Build/ciTestRunner/testCiTestRunner.py`
- Impact: A hand-maintained allowlist can omit a repository test target without
  failing CI. The current inventory misses `linkedActivitySaveCleanup`, leaving
  durable linked-save behavior outside the release gate.
- Test-first evidence / required regression: Add a discoverable test target
  without editing the inventory and require the runner's self-test to fail;
  explicitly require `linkedActivitySaveCleanup` to execute.
- Fix direction: Discover test projects from repository metadata, compare them
  with explicit justified exclusions, and fail on zero, missing, duplicate, or
  unexecuted targets.
- Verification: Confirmed by CI review. Discovery and coverage changes are only
  in unintegrated agent work.

### BUILD-023: Transformed libraries lose authenticated source provenance

- Status: OPEN
- Code: `src/Resources/linux/generate-runtime-provenance.py`,
  `src/Resources/linux/AppImagePackagingSupport.sh`, and
  `src/Resources/linux/MakeAppImageQt6.sh`
- Impact: `patchelf` legitimately changes a copied Debian library, so the final
  bytes cannot equal the package's authenticated digest. Requiring exact source
  bytes rejects valid packaging, while dropping the check leaves transformed
  runtime code unauthenticated.
- Test-first evidence / required regression: Transform a verified fixture
  library, require failure when either source or output changes unexpectedly,
  and accept only a record binding the final path and digest to the verified
  package source and declared transformation.
- Fix direction: Verify the source file against authenticated Debian metadata
  before transformation, then record the transformation plus final output path
  and SHA-256 in provenance and the SBOM.
- Verification: Confirmed by final build review. The source-to-output binding
  remains unintegrated and has not passed a complete package build.

### BUILD-033: Clean builds cannot compile qmake-renamed Bison parsers

- Status: FIXED
- Code: `src/src.pro`, parser-specific `src/{Core,FileIO,Train}/*.tab.h`
  compatibility headers, and
  `unittests/Build/appImagePackaging/testYaccCompatibility.py`
- Impact: Modern Bison makes each generated parser include its original
  `<name>.tab.h`, while qmake immediately renames that header to
  `<name>_yacc.h`. A clean Linux build therefore fails all five parser
  compilations with missing-header errors; parallel builds expose several
  failures at once and cannot produce a release.
- Test-first evidence: The regression generated every declared `YACCSOURCES`
  parser and reproduced five unresolved `.tab.h` includes after applying
  qmake's rename. The independent release build failed the same way in
  `DataFilter`, `JsonRideFile`, `RideDB`, and `WorkoutFilter` before reaching
  the fifth parser.
- Resolution: Each parser source directory now supplies a tracked forwarding
  header from Bison's retained name to qmake's generated name. Keeping the
  wrappers outside the output root also preserves in-source and shadow builds;
  older Bison output remains compatible.
- Verification: The parser regression regenerates every production grammar and
  requires each retained include to resolve after qmake's rename. A clean
  qmake-generated `make -j10` build of all five parser objects passes.

### BUILD-034: Shadow builds link Qwt from the source tree

- Status: FIXED
- Code: `src/src.pro` and
  `unittests/Build/appImagePackaging/testShadowBuildPaths.py`
- Impact: The top-level build writes Qwt to `<build>/qwt/lib`, but every
  application link branch searches `<source>/qwt/lib`. A clean shadow release
  compiles the complete application and then fails at the final link with
  `cannot find -lqwt`; a stale source-tree library could instead be linked.
- Test-first evidence: The regression compared Qwt's declared `DESTDIR` with
  all application release/debug link roots and observed three `PWD` values
  where `OUT_PWD` was required. The independent release build reproduced the
  final-link failure after compiling all objects.
- Resolution: Release and debug links now resolve Qwt relative to `OUT_PWD`,
  matching Qwt's shadow-build destination while retaining in-source behavior.
- Verification: The path regression requires every platform branch to match
  Qwt's output root. A generated shadow-build Makefile now links through
  `-L<build>/src/../qwt/lib`, which contains the freshly built Qwt archive.

### BUILD-035: Credential-free AppImage releases reject their own OAuth status

- Status: FIXED
- Code: `src/Resources/linux/AppImagePackagingSupport.sh` and
  `unittests/Build/appImagePackaging/testAppImagePackaging.sh`
- Impact: The default credential-free Linux package pass verifies that the
  compile-time Strava fallback is absent and returns
  `Strava OAuth compile-time fallback: unavailable`. The build-manifest encoder
  did not recognize that exact successful status, so every such release stopped
  after both independent ELF builds without producing an AppImage.
- Test-first evidence: The manifest regression passed the exact status emitted
  by `require_unconfigured_strava_oauth_build()` to
  `create_appimage_build_manifest()` and reproduced
  `Cannot encode an unknown Strava OAuth status.`
- Resolution: The manifest encoder now maps the verified absent compile-time
  fallback to `strava_oauth_configured=false`. Its fail-closed default still
  rejects every unknown status.
- Verification: The regression passes and the complete AppImage packaging suite
  passes in the release-capable container, including credential, provenance,
  SBOM, keychain, offscreen, and reproducibility checks.

### BUILD-036: Jammy CI cannot install its pinned policy parser

- Status: FIXED
- Code: `.github/scripts/immutable-actions-requirements.lock`
- Impact: The Linux CI job runs on Ubuntu 22.04 and invokes its release-policy
  tests with the host's CPython 3.10. The hash lock covered only CPython
  3.11-3.13 wheels, so a clean Jammy job rejected PyYAML's selected wheel and
  stopped before immutable-action and packaging policy tests could run.
- Test-first evidence: The complete AppImage packaging suite in a clean Jammy
  container selected PyYAML's CPython 3.10 manylinux x86_64 wheel, then pip's
  `--require-hashes` gate rejected its absent SHA-256.
- Resolution: The lock now includes the exact CPython 3.10 x86_64 wheel digest
  published in PyPI's PyYAML 6.0.2 release metadata and documents the actual
  CPython 3.10-3.13 CI range.
- Verification: The same clean Jammy suite now installs the locked wheel and
  passes all immutable-action, credential, provenance, SBOM, keychain,
  offscreen, and reproducibility checks.

### MEM-026: Duplicate activity imports leak the replaced RideItem

- Status: FIXED
- Code: `src/Core/RideCacheImport.cpp` and `src/Core/RideCache.h`
- Impact: `addRide()` overwrites a duplicate vector slot without retiring the
  old object. `addRides()` receives every displaced object from `mergeItems()`
  but discards that list with `Q_UNUSED`. Since `RideItem` has no QObject parent,
  repeated imports leak its metrics, intervals, optional open RideFile, and live
  signal connections back into the cache.
- Test-first evidence: The initial single-import regression observed no model
  reset at all, leaving the displaced object outside the model protocol. The
  batch regression returned all four inputs although only three identities
  survived. After the first retirement implementation, a reset callback that
  destroyed the replacement left `Context::ride` pointing at the detached old
  object. Destructor, reset, selection-signal, and cache-update counters now
  cover those failures plus a duplicate within one batch.
- Resolution: Single and batch imports publish replacements inside the reserved
  model reset and guard every old and incoming item with `QPointer`. The batch
  path consumes and deduplicates the displaced set returned by `mergeItems()`.
  Displaced objects remain alive through model callbacks, are disconnected from
  cache updates, removed from the reverse work list, and queued exactly once on
  the existing deferred garbage path. Current selection is remapped to the live
  same-identity replacement, and only final live incoming rows are refreshed,
  announced, selected, or returned.
- Verification: All three focused lifetime regressions pass strict
  ASan/UBSan/LSan. The complete 320-case RideCache program passes ASan/UBSan and
  ThreadSanitizer, and all 13 bulk-import cases pass both sanitizer
  configurations. The production application links and survives an isolated
  15-second offscreen event-loop smoke test with a disposable home directory.

### DATA-011: CPX freshness trusts source mtime instead of its stored CRC

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`,
  `src/FileIO/RideFileCache.h`,
  `src/FileIO/RideFileCacheIntegrity.cpp`,
  `src/FileIO/RideFileCacheIntegrity.h`, and
  `src/Core/APIWebService.cpp`
- Impact: A CPX is accepted without calculating its source CRC whenever the
  source mtime is equal to or older than the cache mtime. Restored mtimes,
  coarse timestamp resolution, and copied older files can therefore retain
  stale derived data despite the checked CRC implementation. Fast scalar,
  batch, API, rank, and in-memory aggregate paths also bypassed the constructor
  checks and could publish unauthenticated arrays.
- Test-first evidence: RED regressions replaced a source while restoring an
  equal or older mtime, removed it, and constructed a deliberate CRC16
  collision. Separate cases corrupted same-size CPX payloads and digests,
  replaced the CPX between payload extraction and digest verification, exposed
  partial-reader output before final authentication, and exercised best/TIZ,
  API, batch, rank, and aggregate bypasses. Counting devices proved malformed
  or missing CPX files do not hash their source and a valid scalar read uses
  one CPX pass and one source pass.
- Resolution: CPX format 26 stores the stable source byte count and SHA-256
  fingerprint and appends a SHA-256 digest over the complete preceding CPX
  stream. Readers validate the bounded preamble before allocation, stage all
  requested values privately, authenticate one forward stream, and publish
  outputs atomically only after the footer, final size, and source fingerprint
  match. Every constructor, stale check, scalar, batch, API, rank, and
  aggregate path now uses the source-bound reader. Version 25 caches are
  rejected for a one-time rebuild.
- Verification: The 44-case integrity and 34-case refresh/integration suites
  pass normally, under strict ASan/UBSan/LSan with leak detection, and under
  ThreadSanitizer without suppressions. The MinGW header-order syntax check,
  complete Qt 6.8.3 release build, and successful 92-program offscreen matrix
  also pass.
- Residual: The CPX digest detects accidental corruption but is unkeyed and is
  not an authenticity boundary against an attacker who can rewrite local
  cache files. Source and cache validation is a point-in-time guarantee;
  aggregate-set races are tracked by `DATA-015`. Weight and analysis
  configuration are tracked by `DATA-014`, refresh memory duplication by
  `PERF-011`, and the final write-side source/commit window remains documented
  under `DATA-012`.

### DATA-014: CPX reuse does not consistently bind weight and analysis inputs

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`, `src/FileIO/RideFileCache.h`,
  `src/FileIO/RideFileCacheIntegrity.cpp`, and
  `src/FileIO/RideFileCacheIntegrity.h`
- Impact: Item-aware mutable best/TIZ calls verify the current athlete weight,
  but filename and const-item overloads, the single-file mean-max helper, and
  in-memory aggregate reuse can accept source-authentic CPX values after weight
  changes. Zone/profile/configuration inputs that affect derived arrays are not
  represented in the persisted source identity. W/kg and zone-dependent
  results can therefore remain stale while the activity bytes are unchanged.
- Test-first evidence: Regressions create a source-authentic CPX with one
  analysis fingerprint, then substitute weight-, zone-, and setting-equivalent
  fingerprint generations. Cache-current checks, individual best/TIZ reads,
  and batched best reads accept the original generation and reject every
  changed generation. Aggregate tests run the production source-and-analysis
  binding validator and reject both changed source bytes and a changed member
  analysis generation. The CPX integrity test also proves that the persisted
  analysis fingerprint is covered by the authenticated cache digest.
- Resolution: CPX v27 authenticates a SHA-256 fingerprint of analysis-affecting
  inputs alongside the source fingerprint. Its canonical input includes
  activity date, sport and swim mode, weight, power/heart-rate/pace zone ranges
  and thresholds, W'bal formula and tau, and wheel size. Full, partial, batched,
  and aggregate reads require the current fingerprint whenever their result is
  analysis-dependent. Explicitly independent mean-max series may use the
  source-bound cache without resolving athlete analysis state; contextless
  weight-dependent APIs fail closed instead of returning stale values.
- Verification: The focused Qt 6.8.3 suites report 44 refresh and 45 integrity
  tests passing normally, under strict ASan/UBSan/LSan, and in fresh binaries
  linked with ThreadSanitizer. Existing v26 files are rejected and rebuilt once.
  Set-wide aggregate mutation races remain tracked separately by `DATA-015`.

### DATA-015: Aggregate source validation has a set-wide TOCTOU window

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`, `src/FileIO/RideFileCache.h`, and
  `unittests/FileIO/rideFileCacheRefresh/testRideFileCacheRefresh.cpp`
- Impact: Cached aggregate reuse hashes source bindings sequentially. Source A
  can change after its comparison while source B is being hashed, allowing an
  aggregate derived from A's old contents to be returned after A has changed.
- Test-first evidence: A deterministic validation hook replaces source A after
  its source and analysis bindings have passed, before source B is checked. The
  former single-pass validator accepted the mixed source set, producing two
  passing harness cases and one failed race regression.
- Resolution: Aggregate reuse now performs two complete ordered validation
  passes. Every pass recomputes each source's strong content fingerprint and
  resolves its current analysis fingerprint against the generation stored in
  the aggregate. A mutation after an earlier member in the first pass is
  therefore observed and rejected by the second pass before the aggregate is
  returned.
- Verification: The refresh suite reports 45/45 passing normally, under strict
  ASan/UBSan/LSan, and in a fresh ThreadSanitizer-linked build. The guarantee is
  a bounded two-snapshot point-in-time check; a non-cooperating process can
  still mutate a source after the final observation, as with any unpinned
  multi-file read set.

### DATA-016: Activity rename ignores derived-file rename failures

- Status: FIXED
- Code: `src/Core/RideCacheCalendarMutations.cpp`,
  `src/Planning/PlanReplacementJournal.cpp`, and
  `src/Planning/PlanReplacementJournal.h`
- Impact: The activity source is renamed first, but failures to rename notes,
  CPI, or CPX files are ignored and the operation still reports success. A
  destination collision or filesystem error can therefore detach metadata,
  leave stale cache artifacts, or expose an unrelated same-basename sidecar
  under the renamed activity.
- Test-first evidence: Ten new ride-cache rows failed on the baseline. Renames
  committed when unowned CPI, CPX, or notes targets already existed or appeared
  during publication; shared CPI and notes files were not copied to the new
  identity; and the shared-sidecar crash hooks were never reached because those
  files were outside the journal. The required-absence journal regressions were
  compile-RED because the journal had no way to bind a target that must stay
  absent throughout publication and recovery.
- Resolution: Activity files and every existing CPI, CPX, and notes artifact
  are staged and published through one plan-replacement journal. Legacy CPI and
  notes files shared by another activity are copied to the new identity while
  the shared source is retained. Missing derived artifacts add anchored,
  locked must-remain-absent entries to manifest version 2; version 1 journals
  remain recoverable. A destination collision, concurrent appearance, parent
  substitution, staging failure, or crash now either preserves the old
  generation or leaves an explicit recovery journal instead of reporting an
  inconsistent rename as successful.
- Verification: On the final integrated source, the complete plan-replacement
  suite reports 149/149 passing and the complete ride-cache removal and rename
  suite reports 406/406 passing with one expected environment-specific skip.
  Focused strict ASan/UBSan/LSan runs report 13/13 and 37/37 passing with leak
  detection enabled. Fresh binaries confirmed to link `libtsan` report the same
  13/13 and 37/37 under ThreadSanitizer. The sanitizer source files are
  byte-identical to the integrated files.

### METRIC-004: Ride best ranking is sorted in the wrong direction

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp:2846-2884`
- Impact: `rank()` sorts values ascending and returns the first value less than
  or equal to the candidate. For values 100, 200, and 300, a candidate of 250
  is therefore reported as rank 1 instead of rank 2; most non-minimum results
  collapse to the top rank.
- Test-first evidence: A fixed best-value set covers top, middle, bottom, ties,
  a rejected cache row, and the defined `of + 1` last-place result. The old
  ascending loop returned ranks `1, 1, 1, 1, 1, 4` where the expected ranks
  are `1, 1, 2, 2, 4, 5`.
- Resolution: Accepted values are sorted descending and ranked with
  `std::lower_bound`, placing a tied candidate before equal existing values.
  `of` remains the number of accepted cache rows, while insertion below every
  row is reported as `of + 1`.
- Verification: The focused data-driven QtTest regression passes in the remote
  Qt 6.8.3 Docker build and covers top, middle, bottom, ties, rejected-cache
  rows, and the documented `of` semantics.

### PERF-011: Verified CPX refresh duplicates full caches and payloads

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`, `src/FileIO/RideFileCache.h`, and
  `src/FileIO/RideFileCacheIntegrity.h`
- Impact: Verified refresh retains the original `RideFileCache`, a second
  independently parsed cache, and two complete serialized `QByteArray`
  payloads at once. Long activities with large mean-max and distribution
  arrays can create a substantial avoidable memory peak.
- Test-first evidence: A large synthetic refresh records every destination
  write. The pre-fix serializer issued a 96,836-byte payload write, exceeding
  the 65,536-byte bound. A second regression mutates computed cache data after
  verification and requires publication to fail rather than install a payload
  different from the verified generation.
- Resolution: Verification serializes each cache into a fixed-size SHA-256
  digest sink instead of retaining complete payload byte arrays. The accepted
  cache is then serialized directly through a hashing forwarding device to the
  atomic writer in chunks of at most 64 KiB, and its byte count and digest must
  still match the independently recomputed cache before commit.
- Verification: The bounded-write and post-verification-mutation regressions
  pass as part of the 44-test refresh suite normally, under strict
  ASan/UBSan/LSan, and in a fresh ThreadSanitizer-linked build. The independent
  verification cache's numeric tables still coexist temporarily with the
  original computed tables, but the two full serialized payload copies have
  been eliminated.

### DUR-012: CPX refresh can publish torn files and stale in-memory arrays

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`,
  `src/FileIO/RideFileCache.h`, and
  `src/FileIO/RideFileCacheIntegrity.cpp`
- Impact: Direct replacement of a CPX file exposed partial headers and payloads
  to concurrent readers if serialization or the process failed. Failed cache
  reads and repeated refreshes could retain arrays from an earlier activity,
  fixed-size zone arrays were not reliably restored, and the normalized-power
  distribution count was serialized from the wrong array.
- Test-first evidence: Atomic replacement tests preserve an existing file after
  an injected write failure and replace it only after success. Refresh
  regressions clear poisoned arrays, restore every fixed zone size, and prove a
  second refresh does not retain old values. Exact read/growth failures require
  an empty incomplete result.
- Resolution: CPX writes use `QSaveFile` commit semantics. Computation clears
  all published arrays before work, restores fixed zone storage, fixes the
  normalized-power count, and marks a cache complete only after successful
  computation or complete validated loading. Persistence failure leaves valid
  in-memory results available without publishing a torn file.
- Verification: The focused refresh/integration program passes seven normal
  cases and the integrity program passes 31. Final sanitizer and complete-suite
  evidence is recorded in the verification baseline.
- Residual: A cache write failure is still reported through legacy UI code. Its
  process-wide warning state and GUI-thread ownership are tracked separately by
  `THREAD-016`.

### THREAD-017: Mean-max worker completion is opaque to ThreadSanitizer

- Status: FIXED
- Code: `src/FileIO/RideFileCache.h`
- Impact: Mean-max calculation used stack-owned `QThread` subclasses and
  `QThread::wait()`. Qt 6.8.3 documents that wait as a complete Linux join, but
  its unsanitized private wait-condition implementation did not expose the
  happens-before edge to the instrumented application. ThreadSanitizer emitted
  38 apparent cross-test stack, container, and wait-condition races, aborting
  strict validation before it could distinguish application races. This was a
  sanitizer visibility defect rather than evidence that Qt violated its
  runtime contract.
- Test-first evidence: The seven-case CPX refresh/integration program passed
  functionally but produced 38 ThreadSanitizer reports with the original
  `QThread` workers. The reports included recycled `MeanMaxComputer` and
  `RideFile` stack locations plus Qt's internal `QWaitCondition` destruction.
- Resolution: `MeanMaxComputer` now owns a `std::thread`, joins it explicitly,
  and also joins during destruction. Existing synchronous `run()` callers keep
  their behavior, while asynchronous computation now has a sanitizer-visible
  standard join.
- Verification: The unchanged seven-case refresh/integration program and the
  31-case integrity program both pass strict ThreadSanitizer execution with no
  reports.
- Residual: Worker outputs remain disjoint, but the shared source `RideFile`
  must remain immutable for the duration of mean-max computation as required by
  the existing synchronous cache API.

### THREAD-016: CPX write-error reporting races and may open UI off-thread

- Status: FIXED
- Code: `src/FileIO/RideFileCache.cpp`,
  `src/FileIO/RideFileCacheWriteError.cpp`,
  `src/FileIO/RideFileCacheWriteError.h`, `src/Core/Context.cpp`,
  `src/Core/Context.h`, `src/Gui/CacheWriteWarning.cpp`,
  `src/Gui/CacheWriteWarning.h`, `src/Gui/MainWindow.cpp`,
  `src/Gui/MainWindow.h`,
  `unittests/FileIO/rideFileCacheRefresh/testRideFileCacheRefresh.cpp`,
  `unittests/FileIO/rideFileCacheWriteError/testRideFileCacheWriteError.cpp`,
  and `unittests/Gui/cacheWriteWarning/testCacheWriteWarning.cpp`
- Impact: `RideFileCache::refreshCache()` guards its one-time warning with an
  unsynchronized function-static `bool` and constructs a modal `QMessageBox`
  directly on whichever thread performs the refresh. Concurrent refreshes can
  race the flag, and a worker-thread failure can invoke QWidget code outside
  the GUI thread or block background processing indefinitely.
- Test-first evidence: The coordinator regression initially failed to compile
  because no synchronized error model existed, and the integration regression
  then failed to compile because the real refresh path had no injectable
  persistence boundary. A later deterministic race test produced five passes
  and one failure: a second report was incorrectly coalesced while the first
  dispatch was still pending and was lost when that dispatch failed. The
  owner-lifetime test and widget-level test each failed their first build
  because no shared receiver-bound queue or testable GUI warning boundary
  existed. A final reentrancy regression produced 11 passes and one failure
  because a nested report waited for its own dispatch to finish. The bounded
  retry regression then produced 12 passes and one failure after observing four
  dispatch attempts instead of two. Delivery-construction and post-delivery
  exception regressions finally produced 13 passes and two failures by exposing
  a stuck dispatch state and a retained pending callback.
- Resolution: The cache layer now computes before persistence, returns the
  persistence result, retains valid in-memory values after write failure, and
  reports value-only error data through `Context`. A mutex-protected,
  non-blocking coordinator distinguishes dispatch in progress from an accepted
  notification. The first report arriving during dispatch is retained as a
  value-only pending message; a failed dispatch retries that message once
  through the active owner's callback pair, while an accepted dispatch
  coalesces it. Generation checks reject stale retained deliveries, and
  synchronous, exceptional, and reentrant dispatch paths leave a retryable
  state without retaining another caller's callbacks. Qt queues delivery
  against the owning `Context`, so destruction cancels the callback.
  `MainWindow` delegates to a tested GUI helper that shows an owner-bound,
  delete-on-close, non-modal warning.
- Verification: The final coordinator program passes all 15 cases normally,
  under strict ASan/UBSan/LSan, and under ThreadSanitizer, plus 100 consecutive
  normal runs. The real four-worker CPX refresh program passes all eight cases
  in the same three configurations while preserving every computed in-memory
  result after injected write failure. The widget program passes all three
  cases normally and under strict ASan/UBSan/LSan. The complete release matrix
  passes 91 QtTest programs and 3,351 cases with zero failures or blacklisting
  and 12 expected Linux platform skips.
- Residual: Each cache failure remains visible in the warning log, while only
  the first successfully queued failure per `Context` opens a dialog.
  As elsewhere in the cache worker contract, callers must not invoke a raw
  `Context` after its owning athlete has begun destruction.

### SEC-023: External vault mutations bypass credential cache revisions

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/CredentialSettings.h`, `src/Core/Settings.cpp`,
  `src/Cloud/Strava.cpp`, `src/Cloud/StravaTokenRefresh.cpp`,
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`, and
  `unittests/Cloud/stravaTokenRefresh/testStravaTokenRefresh.cpp`, plus
  `unittests/Cloud/stravaOAuthPolicy/testStravaOAuthPolicy.cpp`
- Impact: Cache entries are invalidated by GoldenCheetah's revision sidecar, but
  direct keychain changes by another application do not advance that revision.
  Normal reads can therefore return a stale positive or negative value until a
  local mutation, cache clear, or restart. SEC-018 now bypasses cached misses
  when authorizing cross-file fallback, but ordinary credential reads retain
  the broader stale-cache behavior. A long-lived Strava service also treated
  unchanged authorization metadata as proof that a newly read credential pair
  was unchanged, extending an externally replaced or deleted token past cache
  expiry.
- Test-first evidence: With a deterministic monotonic test clock but no expiry
  behavior, external replacement, deletion, insertion after a cached miss, and
  a backend error after expiry produced four failures and only the three
  setup/memory-only cases passed. A second RED matrix showed that an
  authoritative read still returned a fresh cached positive after external
  replacement or deletion: three cases passed and two failed. The follow-up
  `authoritativeStorageReconciliationObservesSameMetadataCredentialChange`
  regression keeps storage metadata unchanged while replacing and deleting
  the pair, then exercises the coordinator path used by `Strava`; the existing
  storage-reconciliation source contract also forbids restoring the metadata
  fast path.
- Resolution: Persisted positive and all negative cache entries now carry a
  monotonic insertion timestamp and expire after 30 seconds. Lookup checks and
  erases an expired entry under the cache mutex; clock rollback also expires
  it. A failed live refresh remains fail-closed and never revives the stale
  value. GoldenCheetah revision changes still invalidate immediately.
  `ReadPolicy::RequireLiveVault` explicitly bypasses vault-backed positive and
  negative cache entries for legacy-fallback authorization. Pending
  memory-only writes remain visible without expiry but cannot report live-vault
  evidence or authorize fallback. Strava now reconciles every authoritative
  read against the complete in-memory pair and uncertainty state; an active
  record with missing credentials becomes fail-closed pending state instead of
  retaining the previous request token.
- Verification: The final credential program passes 423 cases normally with
  zero failures and seven Linux platform skips. The final SEC-023 cache,
  policy, revision, process-mutation, and serialization matrix passes 29 cases
  under strict ASan/UBSan/LSan and 29 under ThreadSanitizer with no reports or
  races. A full strict sanitizer run passed 422 cases and had one independent
  `THREAD-014` failure, with seven skips and no sanitizer report. Final
  production and test sources pass MinGW64 C++17 syntax checks. The complete
  out-of-source matrix runs 81 QtTest programs: 3,167 cases pass, none fail or
  blacklist, and seven platform cases skip.
- Residual: Ordinary reads may remain stale for up to 30 seconds of monotonic
  runtime, potentially plus system suspend time. A non-cooperating application
  can still mutate the vault immediately after a live read; eliminating that
  race requires backend notifications, generations, or compare-and-swap.
  Pending memory-only values intentionally remain process-lifetime state until
  their transaction is resolved.

### THREAD-014: Contended credential reads fail immediately during migration

- Status: FIXED
- Code: `src/Core/CredentialSettings.cpp`,
  `src/Core/Settings.cpp`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: `CredentialSettings::value()` attempts the per-vault process lock
  with a zero timeout. During concurrent fresh-athlete initialization, one
  process can hold that lock while migrating and durably scrubbing plaintext;
  another process then skips both migration and its immediate read and reports
  the credential as missing even though the first process completes normally.
- Evidence: `freshEnrollmentIsSerializedAcrossProcesses(athlete)` failed once
  during earlier normal verification, failed again in a full strict sanitizer
  run, and reproduced on iteration four of an isolated strict sanitizer loop.
  The failing snapshot contained complete root, profile, and scope claims and
  bindings; only the losing child observed `missing`. The focused enrollment
  API serialization test remains clean, isolating the race to the later
  credential operation lock.
- Test-first evidence: A parent process acquired the exact hashed per-vault
  `QLockFile`, and the child signalled its failed immediate acquisition from
  inside `CredentialOperationGuard`. Before the fix, the owner-release row
  returned `missing` instead of the stored secret, while the timeout row
  returned after 7 ms instead of waiting for the bounded lease. Both rows
  failed deterministically.
- Resolution: Credential reads now pass the named five-second
  `credentialReadProcessWaitMilliseconds` bound to
  `CredentialOperationGuard`. The guard still attempts the process lock
  immediately before waiting. Its process-local mutex remains a non-blocking
  `tryLock`, and mutations continue using the zero-wait default, so reentrant
  calls and writes remain fail-fast. Exact-operation test signals distinguish
  contention, successful admission, and timeout. The live-vault evidence flags
  are cleared before lock acquisition, and the existing backend mutation
  marker and lease checks still run only after admission.
- Verification: The final focused lock, reentrancy, marker, timeout, and
  athlete-enrollment matrix passes all 16 cases normally, under strict
  ASan/UBSan/LSan, and under TSan without sanitizer or race reports. The full
  normal and strict sanitizer credential programs each pass 426 cases with
  zero failures and seven Windows-only skips. Ten consecutive normal and ten
  consecutive strict sanitizer runs of the previously intermittent athlete
  enrollment row pass. Both changed translation units pass MinGW64 C++17
  syntax checks. Independent reviews found no production-code blocker and
  their test determinism findings were incorporated. The complete
  out-of-source matrix runs 81 QtTest programs: 3,170 cases pass, none fail or
  blacklist, and seven Windows-only cases skip.
- Residual: A cross-process read can block its caller for up to five seconds
  and is not cancellable inside `QLockFile::tryLock`. The process-global
  credential mutex is held during that wait, so unrelated credential
  operations in the same process fail immediately. Startup migration can
  accumulate one bounded wait for each independently contended plaintext
  credential. A same-user process can also hold the private lock repeatedly;
  the hardened state directory prevents cross-user interference but not
  same-user denial of service.

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

### MEM-023: File CRC computation leaks its read stream

- Status: FIXED
- Code: `src/FileIO/RideFile.cpp` and
  `unittests/FileIO/rideFileOwnership/testRideFileOwnership.cpp`
- Impact: Every `RideFile::computeFileCRC()` call allocated a `QDataStream`
  without an owner or matching delete. Activity discovery and each CPX cache
  validation or refresh therefore leaked one stream for the process lifetime.
- Test-first evidence: The ownership suite repeated CRC calculation 64 times.
  All 12 functional cases passed, but strict ASan/UBSan/LSan terminated the
  original implementation after reporting 2,048 leaked bytes in 64 allocations
  rooted at `RideFile::computeFileCRC()`.
- Resolution: The stream now has automatic storage duration and is destroyed
  when CRC calculation returns. File opening, reading, and checksum behavior
  are otherwise unchanged.
- Verification: The final 12-case ownership suite passes normally and under
  strict ASan/UBSan/LSan with leak detection. The eight-case concurrent CPX
  refresh suite that originally exposed the leak also passes under the same
  sanitizer configuration.
- Residual: CRC calculation still buffers the complete source file and does not
  distinguish a short read from normal content. Those I/O and resource-limit
  concerns are outside this ownership fix.

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

- Status: FIXED
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
- Test-first evidence: Deterministic trainer and heart-rate rows first failed
  because an invalid local adapter left no scan retry armed. Additional RED
  cases reproduced endless retries for permanent discovery errors, controller
  churn before adapter recovery, stale queued callbacks across stop/start,
  pairing scans that never completed, and recovery of one missing peer
  suspending an already healthy device. Later RED cases showed a deliberately
  disconnected peer being reconnected, a trainer being reported restored
  before GATT readiness, and a slope target being written to the retired
  service and then omitted after recovery.
- Resolution: Adapter errors now suspend device-level reconnect attempts,
  clear their telemetry sources, block stale trainer-control writes, and leave
  only devices with an active connection request pending behind the
  controller's bounded 2-30 second scan backoff. Adapter-invalid recovery and
  each training session after a stop recreate the local-adapter and discovery
  objects; a stack-generation guard rejects queued discovery, error, finish,
  and cancellation callbacks from retired scanners. A configured identity
  must be rediscovered through a valid adapter before its low-energy controller
  is replaced and reconnected with the preserved address type. Trainer
  recovery remains watched until a usable non-heart-rate GATT service is ready,
  and the current slope target is then sent to the replacement service.
  Recovery remains independent per device. Permission, unsupported-platform,
  unsupported-method, and disabled-location errors stop background retries and
  require user action.
- Verification: All 76 BLE lifecycle cases pass normally, in 20 consecutive
  normal runs, under strict ASan/UBSan/LSan, and under TSan with uninstrumented
  Qt modules excluded. MinGW syntax checks pass for all four changed production
  and test translation units. The full Qt 6.8.3 application links and remains
  running in an isolated offscreen smoke test with a clean home directory. The
  complete matrix reports 81 test programs, 3,196 passes, zero failures, seven
  skips, and 81 finish markers.

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

- Status: FIXED
- Code: `src/Core/RideCache.cpp`, `src/Core/RideCache.h`,
  `src/Core/RideCacheStartup.h`, `src/Core/RideDB.y`,
  `src/Core/RideCacheBackgroundSaver.cpp`,
  `src/Core/RideCacheBackgroundSaver.h`,
  `src/Core/RideCacheSaveCapture.cpp`,
  `src/Core/RideCacheSaveCapture.h`,
  `src/Core/RideCacheSaveSnapshot.cpp`,
  `src/Core/RideCacheSaveSnapshot.h`, and
  `unittests/Core/rideCacheSaveSnapshot`
- Impact: Concurrent import/delete/metadata edits can race serialization and
  produce inconsistent or malformed cache JSON, dereference deleted cache
  objects, publish a canceled refresh generation as current, or lose a
  requested save target during refresh or shutdown.
- Test-first evidence: The first focused target did not compile because no
  snapshot API existed. Successive RED regressions then exposed absent
  cancellation-target policy, refresh barriers, pending-generation handling,
  bounded refresh and write waits, injected writer failures, stale-item
  exclusion, timeout target retention, and saver reentrancy protection. A final
  failure-path review also found that a test assertion could abandon a blocked
  writer before releasing its gate; the test was corrected before the complete
  rerun.
- Resolution: RideCache now captures a value-only snapshot on its owner thread
  and serializes it through a FIFO `std::thread` worker with atomic
  publication. Synchronous saves settle the current refresh and every pending
  generation before capture; asynchronous targets are deferred while refresh
  is active and later flushed. Superseded and canceled results remain stale,
  stale or discarded rides are never persisted as clean, and the stable
  notification boundary prevents a refresh from starting between notification
  delivery and capture. Refresh and write waits use aggregate 30-second
  deadlines. A timed-out synchronous waiter abandons only its wait, not the
  queued FIFO write, and later failures remain reportable through `drain()`.
  Shutdown cancels and joins refresh work, flushes retained targets, drains and
  stops the saver, and performs the final safe save.
- Verification: The focused program passes all 29 cases normally, under strict
  ASan/UBSan/LSan with leak detection, and under ThreadSanitizer without
  suppressions. It also passes 20 consecutive normal runs. A clean release
  application and every registered test target compile and link. The exact
  final matrix runs 82 QtTest programs: 3,225 cases pass, none fail or
  blacklist, and seven expected platform-only cases skip on Linux. All three
  new production translation units pass a MinGW64 C++17 syntax check.
  Independent reviews found no remaining production blocker in this change.
- Residual: The OpenData exporter has a separate live-object worker path and is
  tracked by `THREAD-015`. Native Windows and macOS runtime behavior still
  requires their platform CI; the MinGW check validates syntax only.

### DUR-007: Split transactions have no restart recovery journal

- Status: FIXED
- Code: `src/Core/RideCache.cpp`,
  `src/FileIO/AtomicFileWriter.h`,
  `src/FileIO/AnchoredFileSystem.cpp`,
  `src/FileIO/AnchoredFileSystem.h`,
  `src/Gui/SplitActivitySave.cpp`,
  `src/Gui/SplitActivitySave.h`, and
  `src/Gui/SplitActivityWizard.cpp`
- Impact: Runtime failures are rolled back, but a process or power loss between
  publishing outputs, preserving an old backup, and archiving the source can
  leave a recoverable mixture of split files and `.rollback-*` state with no
  automatic reconciliation on restart.
- Test-first evidence: Subprocess regressions crash at every durable save and
  recovery transition, restart with a fresh process, and require one complete
  generation without losing a source, prior backup, or foreign replacement.
  The baseline had no persistent split intent or startup reconciliation path,
  so interrupted publication could not satisfy that contract. Additional
  regressions cover hostile and replaced journals, bounded handles and reads,
  recovery deadlines, same-filesystem publication, and durable-generation
  failures.
- Resolution: Split publication now uses a private, permission-restricted
  transaction directory with identity-bound and durably synchronized control
  records. Source snapshots, prior backups, staged outputs, commit markers, and
  cleanup transitions are validated through anchored handles. Recovery is
  forward-only after the commit boundary, preserves unowned replacements, and
  runs before `RideCache` loads activities. Work, payload, path, deadline, and
  open-handle budgets bound hostile or abandoned journals.
- Verification: `splitActivitySave` passes 104/0/0 normally and 104/0/0 under
  strict ASan/UBSan/LSan. `atomicActivitySave` passes 331/0/0 in both modes;
  `rideFileCacheRefresh`, `planReplacementJournal`, and `rideCacheRemoval` pass
  45/0/0, 149/0/0, and 406/0/1 respectively in both modes. The full Qt 6.8.3
  application links, and an isolated minimal-platform invocation reports
  `GoldenCheetah V3.8-DEV2605 (5012)`.

### DUR-008: Staged-set rollback trusts a mutable target pathname

- Status: FIXED
- Code: `src/FileIO/AtomicFileWriter.h` and
  `unittests/FileIO/atomicActivitySave/testAtomicActivitySave.cpp`
- Impact: GoldenCheetah holds cooperative path locks, but another process can
  replace a newly published target before finalization fails. Rollback removes
  the current pathname and could therefore delete the other process's file.
- Test-first evidence: A deterministic finalizer moves the transaction's
  published activity aside, creates a different file at the target pathname,
  and then fails. The unsafe baseline deleted that replacement; the regression
  observed an empty target instead of the concurrent contents. A pre-existing
  partial-publication test also demonstrated that a callback-created copy has
  no provable identity continuity with its staging source.
- Resolution: Staging files are pinned before their publisher runs. Every
  claimed output is pinned through an anchored target parent and accepted as
  transaction-owned only when its native identity, size, and SHA-256 digest
  match the staging pin. Rollback removes owned outputs through the pinned
  identity. Replaced targets and ambiguous callback-created outputs are retained
  with an explicit error.
- Verification: The focused regression passes, and the complete atomic-activity
  program passes 311/0/0 normally, under strict ASan/UBSan/LSan with leak
  detection, and under ThreadSanitizer without suppressions. SplitActivitySave
  passes 33/0/0 under both sanitizer configurations. The complete production
  application links and reports its version from an isolated minimal-platform
  profile.

### PORT-001: Unix atomic-new publication requires hard-link support

- Status: FIXED
- Code: `src/FileIO/AtomicFileWriter.h`,
  `unittests/FileIO/atomicActivitySave/testAtomicActivitySave.cpp`, and
  `unittests/FileIO/atomicActivitySave/atomicActivitySave.pro`
- Impact: Unix no-replace publication is implemented as `link(2)` followed by
  `unlink(2)`. Athlete libraries on filesystems that reject hard links cannot
  publish new save, split, backup-archive, or deletion transaction files even
  though each staging file is deliberately created in its target directory.
  Activity and backup directories being on different filesystems is not itself
  a deletion failure: deletion copies and verifies the source into the backup
  namespace and performs each atomic move within one directory.
- Test-first evidence: With hard links rejected, six production-path Linux
  contracts failed before the change across split-set, archive, collision, and
  target-identity cases. The expanded fixture also covers create-new save and
  deletion publication, symlink and hard-link collisions, missing staging,
  unsupported native calls, non-fallback native failures, and partial
  link/unlink reconciliation.
- Resolution: Linux publication first uses
  `renameat2(RENAME_NOREPLACE)`, while macOS uses
  `renameatx_np(RENAME_EXCL)`. Linux `ENOSYS`, `EINVAL`, and `EOPNOTSUPP`, plus
  unsupported Unix platforms, retain the no-replace hard-link fallback. Other
  native errors fail closed without a second publication attempt, and the
  existing partial-effect flag continues to drive identity-preserving rollback.
- Verification: The complete atomic-activity suite passes 331/331. The focused
  native, collision, fallback, and rollback matrix passes 16/16 under strict
  ASan/UBSan/LSan and 16/16 under genuine ThreadSanitizer with `libtsan.so.2`
  verified. Linux exercised the native syscalls at runtime; macOS remains
  compile-guarded and awaits native CI/runtime coverage.

### DB-001: VideoSync import uses video-table helpers

- Status: FIXED
- Code: `src/Train/TrainDB.cpp`, `src/Train/TrainDB.h`, and
  `unittests/Train/trainDbVersionSafety/testTrainDbVersionSafety.cpp`
- Impact: Replace can delete a same-path video and update can skip an existing
  videosync row.
- Test-first evidence: The migration-retry and same-path replacement
  regressions first exposed that VideoSync import selected and replaced rows
  through the video-table helpers, allowing one table's row to affect the
  other.
- Resolution: VideoSync import now queries, updates, and replaces only
  `videosync` rows. Same-path video rows remain untouched, and retries update
  the existing VideoSync record instead of inserting a duplicate.
- Verification: `trainDbVersionSafety` passes 35/0/0 normally and 35/0/0 under
  strict ASan/UBSan/LSan.

### DB-002: Workout update does not update average power

- Status: FIXED
- Code: `src/Train/TrainDB.cpp` and
  `unittests/Train/trainDbVersionSafety/testTrainDbVersionSafety.cpp`
- Impact: Edited workouts retain stale `erg_avg_power` metadata.
- Test-first evidence: The regression inserts a workout, changes its average
  power, updates it, and reads the database directly. The baseline retained the
  original value because the update statement did not assign the bound field.
- Resolution: The workout update now writes `erg_avg_power` from its bound
  value together with the rest of the mutable workout metadata.
- Verification: The focused regression is part of the 35/0/0 normal and
  35/0/0 strict ASan/UBSan/LSan `trainDbVersionSafety` runs.

### DB-003: Training-library transaction failures are ignored

- Status: FIXED
- Code: `src/Train/TrainDB.cpp`, `src/Train/TrainDB.h`,
  `src/Train/Library.cpp`, `src/Train/LibraryImportFileStager.cpp`,
  `src/Train/LibraryParser.cpp`, and `src/Train/WorkoutImportBatch.cpp`
- Impact: Partial imports can be reported to the UI as successful.
- Test-first evidence: Regressions inject transaction-start, duplicate, schema,
  import, serialization, copy, collision, and commit failures. The baseline
  could retain earlier database rows or files, emit success signals, accept the
  dialog, or overwrite metadata despite a later failure.
- Resolution: `TrainDB::ScopedLUW` owns rollback unless an explicit commit
  succeeds. Library refresh/import and dialog batches stage file replacements,
  database writes, and `QSaveFile` metadata together; every failure propagates,
  restores owned files and metadata, and suppresses success publication.
  Signals and dialog acceptance occur only after the durable commit boundary.
- Verification: `libraryTransactionSafety`, `libraryImportFileStager`, and
  `libraryParserSerialize` pass 20/0/0, 12/0/0, and 4/0/0 respectively both
  normally and under strict ASan/UBSan/LSan. The integrated 11-program matrix
  passes 151/0/0 in both modes, the full Qt 6.8.3 application links, and an
  isolated minimal-platform run reports `GoldenCheetah V3.8-DEV2605 (5012)`.

### TRN-004: Core-temperature header is written to the RR file

- Status: FIXED
- Code: `src/Train/TrainSidebar.cpp`, `src/Train/TrainSidebarRuntime.h`, and
  `unittests/Train/trainRuntime/testTrainRuntime.cpp`
- Impact: TCR lacks its header and RR can be corrupted by a TCR header.
- Test-first evidence: Round-trip fixtures exercise RR and TCR together and
  with either file absent. The former TCR path selected the RR device, leaving
  the TCR header empty and contaminating RR output.
- Resolution: Auxiliary header selection names the RR and core-temperature
  destinations explicitly; TCR output now constructs its stream on
  `tcoreFile`.
- Verification: The focused Train runtime suite passes 11/11 normally, under
  strict ASan/UBSan/LSan, and in a ThreadSanitizer-linked build. The five
  adjacent Train suites pass 66/66.

### TRN-005: Discard leaves auxiliary recording files behind

- Status: FIXED
- Code: `src/Train/TrainSidebar.cpp`, `src/Train/TrainSidebarRuntime.h`, and
  `unittests/Train/trainRuntime/testTrainRuntime.cpp`
- Impact: `.rr`, `.pos.csv`, `.vo2`, and `.tcr` files remain orphaned.
- Test-first evidence: A temporary recording creates the primary CSV and every
  supported auxiliary artifact. The former discard path removed only the CSV,
  leaving all four sidecars present.
- Resolution: Discard derives and removes the complete `.csv`, `.rr`,
  `.pos.csv`, `.vo2`, and `.tcr` artifact set from the recording path.
- Verification: Covered by the 11/11 normal and sanitizer-clean Train runtime
  suite and the 9/9 adjacent recording-I/O suite. Individual removal failures
  are retained in the helper result but are not yet surfaced in the UI.

### TRN-006: Initial start signal is emitted twice

- Status: FIXED
- Code: `src/Train/TrainSidebar.cpp`, `src/Train/TrainSidebarRuntime.h`, and
  `unittests/Train/trainRuntime/testTrainRuntime.cpp`
- Impact: Consumers reset twice and the first callback observes non-running state.
- Test-first evidence: A `QSignalSpy` contract requires exactly one signal and
  verifies that initialization and the first target are complete when the
  observer runs. A failure row also requires an initial-target failure to emit
  no start signal. The former production path contained two notifications, the
  first before running state was established.
- Resolution: Start establishes running/recording state and timers, applies the
  initial target, and emits one notification only after those steps succeed.
- Verification: Both start rows pass in the 11/11 normal, strict
  ASan/UBSan/LSan, and ThreadSanitizer runs.

### TRN-007: First workout target is delayed by the load timer

- Status: FIXED
- Code: `src/Train/TrainSidebar.cpp`, `src/Train/TrainSidebar.h`,
  `src/Train/TrainSidebarRuntime.h`, and
  `unittests/Train/trainRuntime/testTrainRuntime.cpp`
- Impact: The trainer can retain its previous target for roughly one second.
- Test-first evidence: A fake-controller contract requires the zero-time load
  before start completion, and explicit SLOPE rows distinguish initial workout
  gradient selection from later timer updates. The former start path waited for
  the first load-timer event.
- Resolution: Start calls the shared workout-target path synchronously before
  notification. ERG mode applies the zero-time load, while SLOPE mode initializes
  the current slope from the workout gradient; later timer ticks preserve the
  existing slope update behavior.
- Verification: The focused initial-load, initial-slope, and failure rows pass
  in all three 11/11 test configurations; the 20/20 FTMS target-readiness suite
  also remains green.

### DEV-005: Daum restart leaves the trainer paused

- Status: FIXED
- Code: `src/Train/Daum.cpp`, `src/Train/Daum.h`, and
  `unittests/Train/trainRuntime/testTrainRuntime.cpp`
- Impact: Both pause and restart set `paused_ = true`, preventing later load writes.
- Test-first evidence: The real Daum thread state-machine fixture starts,
  pauses, restarts, stops, and joins the worker. The former restart left the
  test-visible paused state true.
- Resolution: `restart()` clears `paused_` while holding the existing state
  mutex.
- Verification: The lifecycle row passes normally, under strict
  ASan/UBSan/LSan, and under ThreadSanitizer. A separate real no-device leak
  discovered by the sanitizer investigation remains `MEM-027`.

### MEM-027: Daum worker teardown leaks its serial port and timer

- Status: FIXED
- Code: `src/Train/Daum.cpp`, `src/Train/Daum.h`, and
  `unittests/Train/trainRuntime/testTrainRuntime.cpp`
- Impact: Destroying a started Daum controller does not release its unparented
  `QSerialPort` and `QTimer`, retaining their Qt backing allocations for the
  process lifetime.
- Evidence: A strict ASan/LSan no-device lifecycle run reported 1,364 bytes in
  11 allocations rooted at `Daum::openPort()` and `Daum::run()`.
- Test-first evidence: Production-worker tests for failed open, pseudo-terminal
  partial start, explicit stop/join, and destruction during initialization
  reproduced three failures: partial shutdown took 9.34 seconds, destruction
  aborted with `QThread: Destroyed while thread is still running`, and the
  current strict LSan run retained 1,484 bytes in 11 allocations rooted at the
  serial port and timer.
- Resolution: The serial port and polling timer are now stack-owned by
  `Daum::run()`, so every return path destroys them in their worker thread.
  Failed open and failed initialization return immediately, the timer callback
  is context-bound to the worker-owned timer, and `stop()` requests
  interruption, exits the event loop, and joins the worker. The destructor uses
  the same joined shutdown.
- Verification: The complete `trainRuntime` suite passes 14/14 normally and
  the same 14/14 under strict ASan/UBSan/LSan and genuine ThreadSanitizer. Five
  adjacent Train suites pass 66/66, and the production `Daum` translation unit
  compiles. The normal partial-start and destructor paths now complete in the
  suite's approximately two-second total runtime.
- Residual: `Daum` retains the standard `QThread` ownership requirement that
  its controller object be destroyed outside its own worker thread; normal
  controller teardown already satisfies that contract.

### METRIC-001: Missing/cyclic metric dependencies can loop forever

- Status: FIXED
- Code: `src/Metrics/RideMetric.cpp`,
  `unittests/Metrics/rideMetricDependencyGraph/`, and
  `unittests/unittests.pro`
- Impact: Refresh workers repeatedly requeued an unresolvable parent metric;
  a null metric clone crashed the process, and duplicate requests could publish
  a null result after transferring ownership twice.
- Test-first evidence: Production-path fixtures for a missing dependency, a
  self-cycle, and a multi-node cycle did not terminate within three seconds and
  had to be killed. The null-clone row terminated with `SIGSEGV`, while the
  duplicate-request row returned a null metric. Diamond and ordinary acyclic
  graphs provided ordering and shared-dependency controls.
- Resolution: Metric computation discovers the reachable dependency graph,
  propagates missing-dependency invalidity to dependents, and uses Kahn's
  algorithm for a deterministic topological order. Cycles and their dependents
  are omitted instead of requeued. Clones are held by `QSharedPointer` before
  computation, duplicate roots are de-duplicated, and only non-null requested
  metrics are returned.
- Verification: The focused suite passes 9/9 on the integrated branch and the
  same 9/9 under strict ASan/UBSan/LSan and genuine ThreadSanitizer. The
  sanitizer rows also verify that dependency-only clones and returned roots
  have deterministic ownership.
- Residual: Invalid metrics are omitted from the result using the established
  missing-metric contract; no user-facing diagnostic identifies a malformed
  third-party or user metric graph yet.

### METRIC-002: User metrics retain the first athlete Context

- Status: FIXED
- Code: `src/Core/RideCache.cpp`, `src/Core/RideItem.cpp`,
  `src/Metrics/UserMetric.cpp`, `src/Metrics/RideMetric.cpp`, and
  `src/Metrics/RideMetric.h`
- Impact: Closing the first athlete can leave global metrics with a dangling
  context while other athletes remain open.
- Test-first evidence: The new two-athlete regression first observed that the
  compiled `DataFilter` retained the first athlete address after that athlete
  was destroyed. The old registry also had no immutable replacement API with
  which the concurrent lifetime tests could pin one generation.
- Resolution: User formulas are compiled without an athlete `Context` and the
  active ride supplies its context only while the formula is evaluated.
  Clones share the immutable compiled program, so destroying or replacing the
  defining metric cannot invalidate an in-flight evaluation.
- Verification: `userMetricRegistrySafety` passes 8/8 normally and 8/8 under
  strict ASan/UBSan/LSan. The context-retention row destroys the first athlete,
  evaluates with a second athlete, reloads the definition, and verifies the
  second athlete's value throughout. The complete release build links and its
  112 QtTest suites pass 4,632 cases.

### METRIC-003: Global metric reload races other athlete workers

- Status: FIXED
- Code: `src/Core/Context.cpp`, `src/Core/RideCache.cpp`,
  `src/Core/RideItem.cpp`, `src/Metrics/RideMetric.cpp`, and
  `src/Metrics/RideMetric.h`
- Impact: One athlete cancels only its own cache before global metric objects are
  removed while other workers may still use them.
- Test-first evidence: The regression contract could not be satisfied by the
  mutable global vector: readers received borrowed metric pointers while reload
  removed those objects in place. It also lacked one atomic operation for
  publishing metrics, indexes, dependencies, and the schema generation.
- Resolution: The factory now builds a complete immutable registry generation
  and atomically publishes one `shared_ptr` snapshot. Cache refresh and metric
  evaluation pin that generation for their whole operation; reload never
  mutates or frees objects still visible to another athlete worker.
- Verification: The concurrent two-athlete reload suite passes 8/8 normally,
  8/8 under strict ASan/UBSan/LSan, and 8/8 under genuine ThreadSanitizer while
  replacing the registry hundreds of times. Dependency-graph tests pass 9/9
  normally and under strict sanitizers, and `rideCacheSaveSnapshot` passes
  29/29. An exact `1e73b52` Qt 6.8.3 production build reports configured
  Strava OAuth, all 4,632 registered cases pass across 112 suites, and a
  read-only isolated offscreen startup remains live for the full smoke window.

### GUI-001: RideNavigator stores a dangling stack address in QModelIndex

- Status: FIXED
- Code: `src/Gui/RideNavigatorProxy.h`,
  `unittests/Gui/rideNavigatorProxyMapping/testRideNavigatorProxyMapping.cpp`,
  and `unittests/unittests.pro`
- Impact: `mapFromSource` stores `&p`, the address of a local pointer, as
  `internalPointer`; later mapping dereferences invalid stack memory. The heap
  allocated QModelIndex is also leaked and source row zero is excluded.
- Test-first evidence: The production-path proxy test passed only its setup and
  teardown while row zero, complete source/proxy round trips, and reset
  rebuilding all failed. The sanitizer run additionally reported 200 leaked
  bytes in six allocations from the heap-allocated parent indexes.
- Resolution: Child indexes now refer to the corresponding model-owned group
  index, never a stack address or an allocated temporary. `mapFromSource()`
  validates model ownership and bounds, maps source row zero, and uses the
  complete ungrouped source-row domain. Reset and destruction clear every
  owned group row vector and stable group index.
- Verification: The registered production-path suite passes 6/6 normally and
  6/6 under both ASan/UBSan/LSan and ThreadSanitizer. It round-trips every row
  and column in grouped and ungrouped modes and verifies that persistent
  indexes are invalidated and rebuilt on source reset. The production
  `RideNavigator` object also compiles with the change.
- Residual: Group index addresses remain stable for one model generation and
  are deliberately replaced only inside a Qt model reset, which invalidates
  previously issued indexes before their storage is released.

### GUI-002: Ride deletion can retain a deleted current selection

- Status: FIXED
- Code: `src/Core/RideCacheRemoval.cpp` and `src/Core/Context.h`
- Impact: Deleting the final/current ride can leave the deleted object selected;
  some non-current deletions also omit the deletion signal.
- Test-first evidence: The new current and non-current deletion callbacks checked
  `context->ride` synchronously inside `rideDeleted`. Both failed because the
  notifier temporarily selected the already-removed target even though final
  state looked valid.
- Resolution: Removal chooses and validates the surviving/null selection before
  publishing `rideDeleted`, revalidates it after reentrant callbacks, and then
  publishes `rideSelected`. `notifyRideDeleted` no longer mutates selection.
- Verification: Signal order, signal-time state, current/non-current removal,
  all-rides batch removal, and final selection pass in the 231-case removal
  program. Full widget navigation remains tracked by `TEST-005`.

### GUI-003: Power histogram selection guard is inverted

- Status: FIXED
- Code: `src/Charts/PowerHist.cpp`, `src/Charts/PowerHist.h`,
  `src/Charts/PowerHistSelection.cpp`,
  `unittests/Charts/powerHistSelection/testPowerHistSelection.cpp`, and
  `unittests/unittests.pro`
- Impact: The `RideFilePoint*` overload enters its interval loop only when
  `rideItem` is null and then dereferences it. A null item can therefore
  crash, while every normal non-null ride skips the loop and reports all point
  samples as unselected. Selected intervals are missing from the standard
  power histogram even though the time-based W' balance overload is correct.
- Evidence: GCC 13 diagnoses that the loop calls a member function through a
  null `this` pointer. The adjacent time-based overload has the intended
  positive guard.
- Test-first evidence: Calling the production selection path with a null
  `RideItem` terminated with `SIGSEGV`; GCC also diagnosed the null-`this`
  member call. Non-null selected intervals were never visited because the
  inverted branch was skipped.
- Resolution: The point overload delegates to one directly tested helper that
  rejects a null ride or point before iterating selected intervals. A sample is
  selected only when its half-open time span overlaps a selected interval,
  preserving the existing histogram boundary semantics.
- Verification: The registered helper suite passes 6/6 normally and 6/6 under
  both ASan/UBSan/LSan and ThreadSanitizer. It covers null inputs, an empty
  selection, exact start and stop boundaries, multiple intervals, and ignored
  unselected intervals. Both `PowerHist` production translation units compile.
- Residual: Selection continues to use the existing strict overlap test, so a
  sample ending exactly at an interval start or beginning exactly at its stop
  is intentionally not selected.

### MAP-001: Map nearest-point longitude scaling uses degrees as radians

- Status: FIXED
- Code: `src/Charts/MapRoutePointIndex.cpp`,
  `src/Charts/MapRoutePointIndex.h`, and `src/Charts/RideMapWindow.cpp`
- Impact: `cos(latitude)` receives degrees, selecting the wrong route point at
  many latitudes.
- Test-first evidence: Equatorial and positive/negative 60-degree fixtures
  offer one latitude-offset and one longitude-offset route point. The former
  degree-based cosine selected the latitude-offset point for both high-latitude
  rows, producing two failures while the equatorial control passed.
- Resolution: Route lookup converts the query latitude to radians before
  scaling longitude. Equal-distance results retain original route order through
  their source indices.
- Verification: The focused 10-case Qt 6.8.3 suite passes normally, under strict
  ASan/UBSan/LSan, and in a ThreadSanitizer-linked build. The production
  `RideMapWindow` and index translation units also compile in the release
  application build.

### MAP-002: Map mouse movement repeatedly scans the full activity

- Status: FIXED
- Code: `src/Charts/MapRoutePointIndex.cpp`,
  `src/Charts/MapRoutePointIndex.h`, `src/Charts/RideMapWindow.cpp`, and
  `src/Charts/RideMapWindow.h`
- Impact: Every mousemove performs an O(N) point search, causing stalls for
  long/high-frequency activities.
- Test-first evidence: Deterministic 10,000-, 100,000-, and 1,000,000-point
  fixtures count inspected entries per query. The former implementation
  inspected every point in each row and failed all three bounded-work checks.
- Resolution: Each displayed ride is indexed once by latitude. Lookup uses
  binary search to enter the narrow latitude window, examines only nearby
  points, applies the corrected longitude scaling, and returns the original
  RideFile point index. Ride changes clear the index through the existing map
  bridge reset path.
- Verification: The final release build indexed one million synthetic points in
  28 ms and inspected 13 points for the query; the 10k and 100k rows inspected
  one each. All 10 cases pass normally and under both sanitizer configurations,
  and the production map translation units compile. Timings are diagnostic;
  pass/fail is based on returned identity and bounded examined-point counts.

### ARCH-001: Context directly owned session and persistence lifetimes

- Status: FIXED
- Code: `src/Core/AthleteSession.*`, `src/Core/TrainingSession.*`,
  `src/Core/SessionServices.h`, `src/Core/Context.*`,
  `src/Core/ContextSessionServices.cpp`,
  `src/FileIO/RideFileCache.*`, `src/Core/RideItem.cpp`, and
  `unittests/Core/sessionBoundaries`
- Impact: Training state, WebEngine/bridge resources, and CPX persistence-error
  delivery shared one unstructured `Context` lifetime, making ownership,
  thread-bound notification, and isolated testing difficult.
- Test-first evidence: The dependency check initially failed because `Context`
  had no owned session boundaries. After the first extraction, the production
  build also failed on the remaining `&Context::workout` state alias; the
  extended check reproduced that failure deterministically.
- Resolution: `AthleteSession` owns the athlete-scoped WebEngine and persistence
  services, while `TrainingSession` owns mutable workout, media, timeline, and
  run state. `Context` keeps source-compatible delegation methods as a migration
  layer. CPX writes use an injected `AthletePersistenceService`; legacy callers
  that omit it fall back centrally through the owning `Context`, so reporting
  cannot disappear silently. The manual-workout lease uses session accessors
  instead of a `Context` data member.
- Verification: The boundary suite links production `Context.cpp` and covers
  construction/destruction, per-athlete session isolation, compatibility
  wrapper propagation, persistence delegation, lazy bridge creation, and
  profile lifetime through injected service factories. The cache regression
  exercises the exact omitted-service failure path. Remote focused suites
  passed 614 tests with one existing platform skip and no failures. The new
  boundary and cache suites also passed strict ASan/UBSan and TSan runs (54
  tests in each sanitizer configuration). The architecture check passed, and
  the complete Qt 6.8.3/Qwt 6.8.0 application built, linked, and executed its
  `--version` startup path in the constrained remote container.
- Deferred Low: `Context` still exposes broad mutable
  navigation, filter, selection, and comparison state and therefore remains a
  wider service locator. Eliminating that coupling is outside the active
  High/Medium goal. This FIXED status applies only to the scoped session and
  persistence ownership/thread-lifetime extraction. Workout and video-sync
  pointers retain their existing externally owned lifetime contract.

### ARCH-002: Unit tests link private application object files

- Status: FIXED
- Code: `unittests/unittests.pri.in` and the explicit-source projects under
  `unittests/Core`, `unittests/Gui`, and `unittests/Train`
- Impact: Tests depend on build paths/configuration, compile as C++11 while the
  application uses C++17, and omit most parser/training registrations.
- Test-first evidence: A clean out-of-source `calendarData` build compiled its
  test object and then failed at link time because
  `../../../src/CalendarData.o` did not exist. The shared test configuration
  discovered private `.o`/`.obj` files using the application build's
  `OBJECTS_DIR`, and selected C++11 despite the application's C++17 baseline.
- Resolution: The private object discovery, platform extension selection, and
  `GC_OBJS` linker loop were removed. All eight remaining consumers now name
  their production sources and required moc headers explicitly, while the
  shared configuration selects C++17 and rejects any future `GC_OBJS` use at
  qmake time. The aggregate test project now registers the broader parser,
  database, GUI, and training suites independently of application objects.
- Verification: Eight clean, independent qmake builds selected
  `-std=gnu++1z`, compiled without a prebuilt application tree, and passed
  108/108 tests: CalendarData 3, TrainPerspectiveState 6, SeasonOffset 8,
  Utils 3, Season 32, Units 8, ANT lifecycle 9, and ANT burst bounds 39. A
  source-tree dependency scan finds no remaining `GC_OBJS`, object-directory,
  or platform object-extension consumer.
- Residual: These focused tests compile their small production source sets
  directly. Extracting reusable Core/FileIO/Train libraries would reduce
  duplicate compilation and remains a broader architectural optimization, but
  tests no longer depend on private application artifacts or language-mode
  drift.

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

- Status: FIXED
- Code: `src/Cloud/StravaTokenRefresh.cpp`,
  `src/FileIO/GpxParser.cpp`, `src/Train/StravaRoutesClient.cpp`,
  `src/Train/StravaRoutesDownload.cpp`, and
  `src/Train/StravaRoutesDownloadPipeline.cpp`
- Impact: A Routes request on the GUI thread can join an in-flight refresh by
  waiting on a condition variable. Although the leader's real network request
  is bounded to 30 seconds, GUI events are not processed while the follower
  waits, so Close and Abort cannot update the cancellation flag during that
  interval.
- Test-first evidence: A refresh leader is held open while the real Routes
  follower starts from the dialog. A posted Abort must reach the follower and
  complete promptly without waiting for the leader's deadline. Additional
  regressions require completion on the GUI thread, worker-affine destruction
  of the complete network-service object tree, prompt cooperative teardown,
  safe non-joining teardown for a non-cooperative operation, and responsive
  cancellation during pinned reads, GPX parsing, publication, and database
  finalization. Additional RED rows enter the smart-recording interpolation
  loop, exceed a test-bounded generated-point budget, pass an excessive
  high-water mark, and cancel during the initial GPX XML validation scan.
- Resolution: Route listing, downloading, validation, journal publication, and
  database finalization run in owned worker threads. The dialog receives only
  queued completion callbacks, and Abort/Close set a lock-free cancellation
  flag and request thread interruption without pumping nested GUI events or
  joining a network wait. Worker-owned Qt objects are parented to a worker
  context and destroyed on that thread; shared operation leases keep staged
  data alive across queued and reentrant finalization notifications. GPX smart
  recording now caps its high-water mark and total generated representation,
  polls cancellation inside interpolation, and makes the preliminary XML scan
  cooperative.
- Verification: The 48-case Strava Routes pipeline suite passes normally and
  under strict ASan/UBSan/LSan. Its production-composition cases exercise the
  actual dialog slots, asynchronous Strava service tree, reentrant deletion,
  queued cancellation, and worker finalization. The 30-case Routes client suite
  passes, the complete application links, and an isolated minimal-platform
  `--version` smoke test reports `GoldenCheetah V3.8-DEV2605 (5012)`.

### PERF-008: Strava route imports no longer share one database transaction

- Status: FIXED
- Code: `src/Train/StravaRoutesDownload.cpp`,
  `src/Train/StravaRoutesDownloadPipeline.cpp`,
  `src/Planning/PlanBundleImportJournal.cpp`,
  `src/FileIO/AnchoredFileSystem.cpp`, and `src/Train/TrainDB.cpp`
- Impact: Removing network waits from the original long transaction fixed lock
  duration, but each successfully downloaded route now starts and commits its
  own LUW. Importing many routes loses the previous batching benefit.
- Test-first evidence: Multi-route imports initially observed one LUW per route.
  Regressions require one short LUW for every completed bounded batch, no
  download or GPX parsing inside that LUW, one commit for a cancellation-safe
  completed prefix, and complete rollback of both database and published files
  on begin, import, commit, or generation-validation failure. Final regressions
  force a post-handoff partial replacement, replace `trainDB` at the exact
  publication boundary, invoke target validation outside an LUW, and restart
  with both an intact and a same-content, different-generation substituted
  displaced-predecessor artifact.
- Resolution: Downloads are staged before database access in batches of at most
  eight routes and 32 MiB. Pinned staged inputs are parsed and recorded in a
  durable import journal off the GUI thread. A short finalization phase then
  imports every prepared route through one `TrainDB::ScopedLUW`, validates the
  published generation, removes the journal row, and commits once. Cancellation
  preserves a completed prefix, and later batches begin only after cleanup.
  The decision is additionally witnessed by a constant-size anchored marker
  bound to the athlete, workout-root, and `trainDB` generations, so replacing
  the logical database cannot make a committed decision disappear. Publication
  validates that generation around each mutation, preserves `Partial` outcomes
  for forward recovery, and derives deterministic predecessor names from the
  persisted journal identity. Cleanup pins and verifies the predecessor before
  deletion against the native generation fingerprint persisted in the SQLite
  decision, and is retried on restart before bound database completion.
- Verification: The 48-case pipeline suite passes normally and under strict
  ASan/UBSan/LSan, including real multi-route TrainDB commit and rollback,
  batches larger than eight routes, byte limits, partial failures, cancellation,
  and all transaction rollback paths. PlanBundleImport passes 37 cases,
  TrainDbVersionSafety 35, LibraryTransactionSafety 10, and RideCacheRemoval
  406 with one platform skip, and AnchoredFileSystem 89 with 13 platform skips;
  every listed suite passes normally and under strict ASan/UBSan/LSan.

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

### MEM-021: Cross-thread Strava callbacks could target destroyed QObjects

- Status: FIXED
- Code: `src/Gui/AthletePages.cpp`, `src/Cloud/Strava.cpp`,
  `src/Cloud/StravaTokenRefresh.cpp`, and
  `unittests/Cloud/stravaTokenRefresh/testStravaTokenRefresh.cpp`
- Impact: A worker checked a cross-thread `QPointer` and then separately passed
  its raw widget pointer to `QMetaObject::invokeMethod`. Destruction between
  those operations could make the invocation target dangling during settings
  page or application teardown. The same check/use pattern remained in active
  request abort callbacks: the removal worker checked a reply `QPointer` and
  then used the reply as a queued invocation target while its owner thread
  could destroy it.
- Test-first evidence: Commit `92c7c1a` forbade the raw `QPointer` invocation
  target and required an independently owned GUI context before the fix.
  `threadBoundAbortSurvivesTargetDestructionAtDispatch` deterministically
  destroys the guarded target at the abort dispatch boundary.
- Resolution: The worker retains a shared, GUI-affine `QObject` context for the
  queued phase update. The progress-dialog `QPointer` is dereferenced only
  inside the GUI-thread callback. A shared irreversible flag also prevents a
  late visible Cancel action from changing operation state, and the queued
  phase update restores a dialog hidden by a boundary-racing Cancel before
  removing its Cancel control. Network aborts now queue through an independently
  owned, owner-thread relay and dereference the reply only inside that thread.
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

- Status: FIXED
- Code: `src/Cloud/StravaTokenPublication.cpp`,
  `src/Cloud/StravaCredentialPublisher.cpp`,
  `src/Cloud/StravaCredentialDurability.cpp`, and `src/Cloud/Strava.cpp`
- Impact: Refresh-token rotation must persist the new refresh token before the
  corresponding access token. If the later access-token or timestamp write
  fails, the coordinator correctly leaves authorization pending, but
  production does not schedule recovery. The in-process coordinator still
  knows the observed pair and the publication primitive can retry, but a
  restart loses that evidence and requires reauthorization or disconnection.
- Test-first evidence: One-time access-token and timestamp failures leave a
  complete pending package, then reconstruct the coordinator against the same
  settings and secure store. Further regressions terminate a child process at
  each durable transition, distinguish operations that never reached the
  provider from unknown remote outcomes, reject transiently unreadable state,
  and resume both local and confirmed-remote revocation cleanup. A final
  restart regression preserves the vault grant after an indeterminate remote
  revocation, requires the stale transaction fence to retire, and then starts
  a new explicit local-cleanup generation. Follow-up regressions
  `revocationConflictRecoveryPreservesNewerGrantUntilExplicitCleanup` and
  `publicationConflictRecoveryRetiresFenceForExplicitRetry` cover newer-vault
  conflicts after restart and require a new explicit generation to proceed.
- Resolution: Every grant mutation has an account-bound transaction id and
  generation. An anchored, atomically replaced journal records the previous
  authorization state, mutation kind, remote-transition boundary, and an
  authenticated pending token package before publication can advance. Startup
  recovery reopens that exact generation, keeps authorization pending, and
  either completes all credential, timestamp, state, revision, and sync writes
  or leaves the journal intact for another retry. Incomplete or unreadable
  evidence fails closed instead of admitting API requests. An indeterminate
  revocation with retained credentials remains durably `revocation_pending`
  with remote-grant uncertainty, but its obsolete journal generation retires
  after that state is durable. Authenticated use remains blocked while an
  explicit retry or local-only cleanup can acquire a new fenced generation.
  Confirmed-revocation cleanup stores its expected token and removal mode only
  in the secure pending credential package and replays the same CAS after
  restart. A conflicting newer pair is preserved while the obsolete journal
  retires fail-closed. Publication conflicts follow the same supersession rule
  instead of permanently retaining a non-idle journal.
- Verification: `stravaCredentialDurability` passes 26 cases normally and
  under strict ASan/UBSan/LSan. Together with CredentialSettings (439 with
  seven platform skips), athlete migration (116), OAuth policy (73), account
  removal (21), and token refresh (55), the package passes 730 cases with zero
  failures in both configurations. The complete application also builds and
  links.

### DUR-010: Credential-scope mirror failure is not propagated

- Status: FIXED
- Code: `src/Core/Settings.cpp`, `src/Core/Settings.h`,
  `src/Core/CredentialSettings.cpp`, `src/Core/CredentialSettings.h`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: `mirrorCredentialScope()` logs a failed system-settings sync but its
  callers still cache and return the scope identifier. A later legacy or
  pre-initialization path can then select a different scope after restart,
  while credential migration may already have written to the first scope.
- Test-first evidence: The fresh-enrollment fault matrix originally rejected
  the first root, global-scope, athlete-profile, or athlete-scope authority
  publication and exposed local identities that could not be recovered. Its
  eight global and athlete rows now require the failed attempt to leave the
  vault empty and plaintext intact, then require deterministic recovery both
  in the same process and after reconstructing `GSettings`. Separate exact
  settings transaction tests inject file and directory synchronization
  failures into the shared durability primitive and require fail-closed
  results.
- Resolution: `mirrorCredentialScope()` and its invocation-local scope cache
  were removed. Global and athlete scope selection now uses a checked,
  two-phase external authority enrollment: a canonical location-bound intent
  and claim must be durably published before local metadata is exposed, and
  the authority record is completed only after the exact local binding is
  durable. Every enrollment, binding, and completion failure returns an empty
  scope, so callers cannot write the vault, migrate a credential, or scrub its
  plaintext source prematurely. Pending authority state supports deterministic
  same-process and restart recovery.
- Verification: `interruptedFreshEnrollmentRecovers` passes all eight
  write-failure and recovery rows, including global and athlete mappings. The
  complete credential suite passes 426 cases normally, under strict
  ASan/UBSan/LSan, and under ThreadSanitizer, with no failures and seven
  platform-contract skips per run.
- Residual: A durability failure can leave an authenticated pending authority
  intent on disk, but it cannot authorize credential access until the complete
  local tuple is verified. Recovery deliberately retries that pending intent
  instead of generating a second scope.

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

- Status: FIXED
- Code: `src/Cloud/StravaTokenRefresh.cpp`,
  `src/Cloud/StravaCredentialPublisher.cpp`, and
  `src/Cloud/StravaCredentialDurability.cpp`
- Impact: Static mutexes, registries, epochs, and request permits coordinate
  service clones only inside one GoldenCheetah process. Two processes using the
  same athlete profile can concurrently rotate, revoke, install, or publish a
  grant and defeat the otherwise serialized state machine.
- Test-first evidence: Independent child processes overlap refresh, OAuth, and
  removal against one disposable account and secure-store fixture. Each child
  reports the generation and credentials it observed; the regression requires
  a single durable order and rejects stale publication. Namespace-swap tests
  also replace the lock and journal parents at mutation boundaries.
- Resolution: Refresh, OAuth installation, and removal acquire one private,
  account-derived interprocess lease before reading grant state or crossing a
  provider boundary. The lease owns an anchored journal generation and every
  transition revalidates its transaction id and generation before publication.
  In-process coordination remains the fast path, while the durable generation
  fences independent processes and restart recovery. Replaced lock or journal
  namespaces fail closed.
- Verification: The subprocess serialization and generation-fencing rows pass
  in the 26-case durability suite normally and under strict ASan/UBSan/LSan.
  The adjacent 55 refresh, 21 removal, and 73 OAuth policy cases pass in both
  configurations.

### THREAD-011: Started Strava settings commits can block callers indefinitely

- Status: FIXED
- Code: `src/Cloud/StravaCredentialPublisher.cpp`,
  `src/Cloud/StravaSettingsCommit.cpp`, and
  `src/Cloud/StravaCredentialDurability.cpp`,
  `src/Core/Settings.cpp`, `src/Core/CredentialStoreQtKeychain.cpp`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp`
- Impact: Cancellation and deadline handling can abandon an operation that has
  not started on the settings thread. Once the GUI thread begins a credential
  write, the caller waits for its definitive result even after the deadline.
  This avoids reporting timeout while a mutation can later commit silently,
  but an indefinitely blocked credential backend can hang a worker or
  application teardown.
- Test-first evidence: A storage callback blocks only after it has started.
  Cancellation and the deadline must return a tracked pending result promptly,
  owner teardown must remain bounded, and a reconstructed coordinator must
  resolve the same transaction. Additional rows race late pending-state writes
  against a newer mutation generation and require GUI-originated credential
  work to execute outside the application thread. A deterministic native-job
  hook also queues keychain completion on the application thread while that
  thread starts settings reconfiguration; the wait must process completion
  before the shortened backend deadline instead of freezing the event loop.
  `applicationThreadKeychainReentrancyDefersSettingsReconfiguration` starts the
  native job directly on the application thread, reenters athlete settings
  initialization from its nested loop, and requires that initialization to
  return without touching `QSettings` until the owning suspension unwinds.
- Resolution: Credential storage runs on a dedicated owned worker rather than
  the GUI/settings owner thread. Before dispatch, the durable coordinator
  records the transaction identity and pending phase. An unstarted callback can
  be abandoned; a started callback that exceeds its deadline returns only an
  explicit pending result backed by that journal. Late completion is accepted
  only while its worker and mutation generations remain current, so it cannot
  cross a newer grant mutation. Shutdown interrupts and joins cooperative work;
  a wedged native callback is detached from future generations without blocking
  the caller or allowing untracked publication. Application-thread settings
  reconfiguration now releases the settings mutex and runs a restricted,
  timer-bounded event loop while credential backends remain suspended. It
  reacquires the mutex and rechecks the suspension count before any `QSettings`
  object can be accessed, replaced, or destroyed by that reconfiguration. A
  suspension owned by the current application-thread stack is detected
  separately and causes reentrant initialization to defer immediately, avoiding
  a nested-loop cycle in which only the blocked outer frame can clear it.
- Verification: The started-commit deadline, generation fence, worker-affinity,
  clean shutdown/restart, and recovery rows pass normally and under strict
  ASan/UBSan/LSan. The full six-suite package passes 730 cases with zero
  failures and seven platform-contract skips in both configurations.

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

- Status: FIXED
- Code: `src/Core/main.cpp`, `src/src.pro`,
  `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/MakeAppImageQt6.sh`,
  `.devcontainer/package-appimage.sh`, `appveyor/linux/after_build.sh`, and
  `appveyor.yml`
- Impact: `GC_SOURCE_REVISION` is checked only for hash syntax. Packaging does
  not prove that the commit exists, the worktree was clean, or the supplied
  binary was built from that tree. A plausible but incorrect revision can be
  written beside an unrelated image.
- Evidence: The local release workflow preserves a mode-0600 sidecar and hashes
  the transferred AppImage, but the repository packager does not bind the
  revision to a raw-binary hash or place a verifiable manifest in the image.
  AppVeyor does not generate the same sidecar.
- Test-first evidence: The first packaging test failed because the manifest
  creation API did not exist. A second RED case failed because release
  promotion had no implementation. The completed suite rejects revision A
  binaries claimed as revision B, unknown/non-HEAD revisions, tracked and
  untracked source changes, malformed binary reports, unknown OAuth state,
  tampered images and sidecars, and failed post-publication durability syncs.
- Resolution: Commit `1f57d86` gives the ELF a strict build-provenance command
  and injects the full source revision during qmake configuration. All Linux
  packagers now require an existing clean HEAD, match it to the ELF report,
  hash the raw ELF, record toolchain and boolean OAuth state, embed the
  non-recursive manifest, append the final AppImage hash to a mode-0600
  sidecar, and verify the extracted copy. AppVeyor publishes the same sidecar.
  Promotion copies verified immutable artifacts and swaps one generation
  symlink so `latest` and `previous` change together; a late verification or
  sync failure restores the former pointer.
- Verification: The packaging helper suite passes, malformed qmake revisions
  are rejected, and the production `main.cpp` compiles with the expected
  revision embedded. A clean release build of `1f57d86` reported GCC 13.3.0,
  Qt 6.8.3, C++17, the exact commit, and configured OAuth. Its 366 MiB AppImage
  passed embedded/sidecar hash verification, libsecret and offscreen gates,
  local atomic promotion, and a ten-second isolated-container GUI smoke test.

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

- Status: FIXED
- Code: `src/Cloud/StravaClientCredentials.*`,
  `src/Cloud/AddCloudWizard.*`, `src/Cloud/OAuthDialog.*`,
  `src/Cloud/Strava.cpp`, `src/Cloud/StravaOAuthPolicy.*`,
  `src/Core/CredentialSettings.cpp`, `.github/workflows/ci.yml`,
  `appveyor.yml`, and `util/add_secrets.ps1`
- Impact: Public release builds no longer receive or require a shared Strava
  client secret. Each athlete supplies a Strava client ID and secret at
  runtime; the pair is stored atomically through the platform credential vault
  and never through ordinary QSettings. A compile-time fallback remains only
  for an explicitly requested private personal build.
- Evidence: Runtime credentials take precedence. An unavailable vault or an
  invalid runtime record fails closed without falling back, and blank UI fields
  do not delete a record. Removal requires the dedicated, confirmed Remove
  command. Authorization-code exchange, refresh, and revocation all add the
  runtime secret to their redaction paths; STRAVA_DEBUG logs only the already
  redacted token-failure message. Public GitHub Actions and AppVeyor workflows
  contain no `GC_STRAVA_CLIENT_SECRET` injection, while the release gate accepts
  a runtime-only binary with no compile-time fallback.
- Test: `tst_stravaClientCredentials` passes 14 cases normally and under
  ASan/UBSan/LSan from a shadow build; `testStravaOAuthPolicy` passes 68 cases;
  the focused CredentialSettings selection passes 41 cases; Strava account
  removal passes 21 cases; and both public-release credential and AppImage
  packaging shell suites pass. The modified production translation units also
  compile with a credential-free build configuration.

## Low

As of 2026-08-05, open Low-severity findings are deferred and excluded from
the active remediation goal. They remain documented for later prioritization;
`DEFERRED` does not mean fixed or accepted as harmless.

### PERF-010: A valid zero activity CRC remains an unknown sentinel

- Status: DEFERRED
- Code: `src/Core/RideItem.cpp:538`
- Impact: ISO-3309 CRC value zero is valid, but `RideItem` treats every stored
  zero as unknown. An unchanged activity with such a checksum is conservatively
  refreshed whenever its timestamp changes.
- Test: Store a known-valid zero checksum for non-empty bytes, touch the
  unchanged source, and require no redundant refresh.
- Fix direction: Persist a separate CRC-valid flag or optional state. Treat
  legacy zero values without that state as unknown once.

### PERF-009: OpenData capture can monopolize the GUI thread

- Status: DEFERRED
- Code: `src/Cloud/OpenData.cpp`, `src/Core/RideDB.y`, and
  `src/Cloud/OpenDataSummaryStatistics.cpp`
- Impact: The thread-safety fix for `THREAD-015` deliberately captures the
  athlete graph and computes export summaries on its owner thread. Large
  athlete histories can therefore make the application unresponsive while
  refresh generations settle and paths, metadata, source SHA-256 hashes,
  activity parsing, CSV data, distributions, and summary statistics are
  collected, even though archive construction and upload continue in a worker.
- Evidence: The capture state machine processes the selected ride set
  synchronously before releasing the immutable request to the worker. Current
  tests prove owner-thread access and snapshot ordering, but do not bound
  event-loop latency or memory growth for production-sized histories.
- Test: Drive capture with a large deterministic ride set while recording event
  processing latency and peak memory. Require bounded owner-thread work per
  event-loop turn and bounded cancellation latency inside a source operation,
  while preserving refresh-before-manifest, manifest-before-summary, and
  source-identity guarantees from `THREAD-015`.
- Fix direction: Split capture into resumable owner-thread batches, or publish
  an immutable cache snapshot maintained outside the export path. Revalidate
  every captured source before publication and keep all athlete graph access on
  its owning thread.

### PERF-012: Linked-removal peer staging duplicates the serialized activity

- Status: DEFERRED
- Code: `src/Core/LinkedActivityRemovalJournal.cpp`,
  `src/FileIO/JsonRideFile.y`, and `src/FileIO/AtomicFileWriter.h`
- Impact: JSON save already materializes the complete survivor payload, and the
  atomic helper retains the prior file bytes for compensation. The journal peer
  writer additionally appends every serialized byte to another `QByteArray`
  before publishing its durable staging file. A large linked activity therefore
  adds one full-payload allocation on the GUI thread and can amplify memory
  pressure during deletion.
- Evidence: `JournalPeerWriter::write()` copies each successful delegate write
  into `accumulated_`, and `commit()` passes that complete buffer to a second
  atomic writer. The crash tests prove byte equality but do not bound peak
  memory.
- Test: Feed a large deterministic payload through an instrumented peer writer
  in small chunks and record its maximum retained bytes. Require journal-owned
  buffering to remain bounded by a fixed chunk while preserving the existing
  crash matrix and exact old/new snapshots.
- Fix direction: Open a create-new staging writer alongside the peer's atomic
  replacement, tee and hash each serializer chunk into both, flush and publish
  the staging file first, update the manifest, and only then commit the peer.
  Remove `accumulated_`; broader whole-JSON serialization can be addressed
  separately.

### TEST-001: Strava disconnect UI lifetime coverage is source-only

- Status: DEFERRED
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

- Status: DEFERRED
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

### TEST-003: Compressed OpenData reader suffixes lack end-to-end coverage

- Status: DEFERRED
- Code: `src/Cloud/OpenData.cpp`,
  `src/Cloud/OpenDataCaptureUtils.cpp`, and
  `src/FileIO/CompressedActivityFile.cpp`
- Impact: The export path derives an uncompressed temporary filename from
  `.fit.zip` or `.json.gz`, verifies and copies the compressed bytes, extracts
  them, and asks `RideFileFactory` to select a reader from that temporary
  suffix. Helper and decompressor tests cover the pieces, but a future suffix
  or handoff regression could make compressed activities fail only during a
  real OpenData export.
- Test: Build representative minimal FIT-in-ZIP and JSON-in-Gzip activities in
  a private temporary workspace, run the production snapshot/decompression
  handoff, and require `RideFileFactory` to parse both through the derived
  `.fit` and `.json` paths without touching athlete data.
- Fix direction: Expose the source-to-parser snapshot operation behind a small
  injectable boundary and add an integration test using the real
  `CompressedActivityFile` and `RideFileFactory` implementations.

### TEST-004: Concurrent credential enrollment coverage is timing-sensitive

- Status: DEFERRED
- Code: `src/Core/CredentialSettings.cpp:1608-1661`,
  `src/Core/CredentialSettings.cpp:4715-4805`, and
  `unittests/Core/credentialSettings/testCredentialSettings.cpp:16963-17341`
- Impact: The full matrix can intermittently fail while comparing the two
  process snapshots from concurrent fresh athlete enrollment. The current
  whole-JSON `QCOMPARE` elides the differing suffix, so a failure does not show
  which persisted field diverged and cannot distinguish a product consistency
  bug from a test-observation race.
- Evidence: The unchanged DATA-011 candidate failed the athlete row once in
  the full matrix and once in an isolated rerun, then passed a JUnit rerun,
  five consecutive isolated reruns, and the next complete matrix: two failures
  and seven passes without source or binary changes.
- Test: Compare and report every parsed enrollment field independently, then
  use deterministic barriers around the root, profile, and scope transactions
  to reproduce the divergent schedule.
- Fix direction: First make the failure diagnostic and schedule deterministic.
  If the snapshots reflect real intermediate product state, extend the
  enrollment transaction or publish a committed generation; otherwise move
  the observation behind the protocol's actual completion boundary.

### TEST-005: Activity deletion caller workflows lack widget-level coverage

- Status: DEFERRED
- Code: `src/Gui/MainWindow.cpp`, `src/Charts/CalendarWindow.cpp`,
  `src/Gui/BatchProcessingDialog.cpp`, `src/Gui/PlanWizards.cpp`,
  `src/Gui/SplitActivityWizard.cpp`, and `src/Planning/PlanBundle.cpp`
- Impact: Core deletion statuses, rollback, dirty-peer rejection, and batch stop
  semantics have deterministic unit and sanitizer coverage. Extracted contracts
  also cover Save/Discard preflight, Save Single/Save on Exit lifetime, split
  source identity, deletion owner identity, Repeat Plan owner lifetime, and
  import-reader retention. The real surrounding widgets are still verified only
  by compilation and manual use. A UI refactor could hide `RecoveryRequired`,
  close Repeat Plan with an incorrect
  result, retry a committed split, or bypass Save/Discard/Cancel without failing
  the core suite.
- Test: Use disposable athlete profiles and injectable removal/copy services to
  cover MainWindow and Calendar linked Save/Discard/Cancel, batch cleanup and
  recovery summaries plus remaining rows, Repeat Plan replacement and partial
  copy, Split preflight and post-commit recovery, and PlanBundle partial import.
- Fix direction: Extract the orchestration services from widgets, retain the
  real Qt dialogs in a small offscreen lifecycle suite, and assert both visible
  state and disposable on-disk results without touching a user profile.

### TEST-006: Linked-deletion tests stub production metadata persistence

- Status: DEFERRED
- Code: `unittests/Core/rideCacheRemoval/RideCacheRemovalTestStubs.cpp`,
  `unittests/Core/rideCacheRemoval/testRideCacheRemoval.cpp`,
  `src/Core/RideItem.cpp`, and `src/Gui/SaveDialogs.cpp`
- Impact: The focused suite now thoroughly checks deletion state-machine and
  reentrancy invariants and persists deterministic old/new peer bytes through
  the real journal writer, but its link setters and `saveActivity()` serializer
  remain test stubs. It does not load and rewrite reciprocal metadata through a
  production `RideItem`, `JsonFileReader`, save processors, and cache signals,
  so that complete integration contract is not yet proved.
- Test: Create a disposable athlete with two reciprocal real JSON activities and
  execute production setters and saves through each deletion failpoint. Reopen
  the files and cache after success, rejection, rollback, and recovery-required
  outcomes and verify both persisted links and filenames.
- Fix direction: Keep the fast failpoint unit suite, then add a small production
  integration fixture around real RideItem loading and atomic save services.

### TEST-007: Synchronous model-owner destruction leaves Qt removal bookkeeping

- Status: DEFERRED
- Code: `unittests/Core/rideCacheRemoval/testRideCacheRemoval.cpp`,
  `unittests/Core/rideCacheRemoval/RideCacheRemovalTestStubs.cpp`, and
  `src/Core/RideCacheRemoval.cpp`
- Impact: The single- and batch-removal lifetime regressions deliberately delete
  `RideCache` synchronously from
  `QAbstractItemModel::rowsAboutToBeRemoved`. Qt 6.8.3 then retains two 64-byte
  `QArrayData` allocations from each interrupted model-removal operation. ASan
  and UBSan find no invalid access, all functional assertions pass, and the
  other rows are leak-clean, but these two adversarial rows cannot pass strict
  LSan without hiding a broad Qt allocation signature.
- Evidence: The current complete 231-case instrumented run reaches zero test
  failures with leak detection disabled for this known case. A fresh strict-LSan
  rerun of the two affected rows still reports 256 bytes in four direct
  allocations. Separate strict-LSan
  runs isolate 128 bytes in two allocations to each of
  `modelSignalOwnerDestructionDoesNotContinue(cache-rows-about-to-be-removed)`
  and
  `batchModelSignalOwnerDestructionDoesNotContinue(cache-rows-about-to-be-removed)`.
  The same binary's other six single- and batch-owner-destruction rows and every
  other test function pass with leak detection enabled.
- Test: Reduce the case to a minimal Qt model that is synchronously destroyed by
  its `rowsAboutToBeRemoved` listener and run it against supported Qt versions.
  Keep the production lifetime row under ASan/UBSan even if Qt documents direct
  sender destruction during this signal as unsupported.
- Fix direction: Prefer deferred owner destruction in real UI shutdown paths and
  either adopt an upstream Qt fix or redesign the model transaction so its owner
  cannot be synchronously destroyed mid-notification. Do not suppress all
  `QArrayData::allocate` leaks.

### GUI-008: Repeat Plan cleared an outer navigation guard

- Status: FIXED
- Code: `src/Gui/AthleteTab.h`, `src/Gui/PlanWizards.cpp`, and
  `src/Charts/CalendarWindow.cpp`
- Impact: Calendar disabled automatic view switching around the Repeat Plan
  dialog, but the wizard unconditionally set the shared boolean to false before
  returning. A nested selection callback could switch views inside the caller's
  still-active guarded scope.
- Resolution: AthleteTab exposes the current guard value. Both the wizard scope
  and the Calendar caller retain a guarded original tab and restore that exact
  prior value instead of assuming false.
- Verification: The 27-case Repeat Plan contract passes strict
  ASan/UBSan/LSan; nested real-widget behavior remains part of `TEST-005`.

### GUI-009: Batch save failures were labelled as user cancellation

- Status: FIXED
- Code: `src/Gui/BatchProcessingDialog.cpp`
- Impact: A false Save/Discard preflight result includes reported I/O failures,
  but the final status always said the user aborted processing, obscuring a real
  write failure.
- Resolution: The shared terminal status now says processing stopped without
  inventing a cause; the originating save warning retains the actual error.
- Verification: The complete production application compiles; visible batch
  status coverage remains part of `TEST-005`.

### BUILD-008: Qt 6.8.3 reports impossible QVariant inline-storage overflows

- Status: DEFERRED
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

- Status: DEFERRED
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
- Code: `src/Resources/linux/AppImagePackagingSupport.sh`,
  `src/Resources/linux/MakeAppImageQt6.sh`,
  `appveyor/linux/after_build.sh`,
  `.devcontainer/package-appimage.sh`, and
  `unittests/Build/appImagePackaging/testAppImagePackaging.sh`
- Impact: The Qt build provided `libqoffscreen.so`, but `linuxdeployqt` bundled
  only `libqxcb.so`. The packaged GUI therefore aborted during display-free
  release and CI smoke tests, forcing every AppImage check to depend on a live
  X11 session and repeatedly obscuring real startup regressions with the same
  packaging error.
- Test-first evidence: A direct launch with a clean profile,
  `QT_QPA_PLATFORM=offscreen`, and no display exited 134. Qt reported that the
  offscreen platform plugin could not be found and listed xcb as the only
  available backend. Inspection confirmed that the build image contained the
  plugin while the generated AppDir did not. A later release attempt exposed
  that the first fix covered only the local packager: the devcontainer image
  again exited 127 with xcb as its only platform. The expanded packaging test
  then failed RED because no shared offscreen installer existed.
- Resolution: Shared packaging support now resolves Qt's plugin directory
  through `qmake`, rejects a missing source or linked destination, copies and
  verifies `libqoffscreen.so`, and runs the finished image for ten seconds with
  a disposable HOME, no display, extraction mode, and software rendering. The
  local, AppVeyor CI, and devcontainer packagers all call both shared gates
  after `linuxdeployqt`; a packager can no longer silently omit the plugin or
  skip the runtime check.
- Verification: All five affected shell files pass `bash -n`. The focused
  packaging program exercises successful and missing-plugin installation,
  accepted timeout, rejected startup failure, and all three packager contracts,
  then passes completely. A rebuilt devcontainer AppImage passes its embedded
  Strava OAuth, Linux keychain, and Qt offscreen runtime gates; its AppDir
  contains 260 files, one more than the failing package.

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

- Status: DEFERRED
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

- Status: DEFERRED
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

- Status: DEFERRED
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

- Status: DEFERRED
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

### BUILD-024: AppImage verification executes the untrusted candidate runtime

- Status: FIXED
- Code: `src/Resources/linux/AppImagePackagingSupport.sh`
- Impact: Manifest, SBOM, OAuth, and keychain inspection invoke a candidate
  AppImage with `--appimage-extract`. A malformed or substituted runtime can
  execute arbitrary code before the release payload has been authenticated.
- Regression test: Build a Type 2 fixture whose ELF runtime leaves an execution
  marker, inspect it through every pre-trust verifier, and require successful
  SquashFS extraction without creating the marker.
- Resolution: Candidate inspection now copies the regular AppImage into a
  private snapshot, authenticates the source and snapshot hashes around that
  copy, parses the Type 2 offset without executing the runtime, and extracts
  the snapshot with `unsquashfs`. A changed snapshot removes the extracted tree
  and fails closed.
- Verification: Both executable-runtime and same-size in-place mutation
  fixtures pass with real `mksquashfs` and `unsquashfs`; the complete AppImage
  packaging fixture also passes.

### BUILD-025: AppVeyor dependency setup dirties its source worktree

- Status: FIXED
- Code: `appveyor/linux/install.sh`, `appveyor/linux/before_build.sh`, and
  `appveyor.yml`
- Impact: Linux setup creates `D2XX`, `srmio`, and `python-source` below the
  repository root. The release clean-tree gate consequently rejects the build
  inputs that its own installer created.
- Regression test: Run dependency path setup against a temporary repository and
  require every generated or downloaded path to remain outside the worktree.
- Resolution: Linux setup uses one bounded external input root and passes each
  authenticated dependency path explicitly to configuration and packaging.
  Release source trees remain clean.
- Verification: Release-hardening tests require every generated dependency
  path to resolve outside the repository and reject source-tree inputs.

### BUILD-026: AppVeyor restores release dependencies into non-empty targets

- Status: FIXED
- Code: `appveyor.yml` and `appveyor/linux/install.sh`
- Impact: Whole extracted dependency directories are cached, while the
  fail-closed extractor requires an empty target. A cache hit can therefore
  make the next release setup fail or mix stale and current payloads.
- Regression test: Assert that AppVeyor does not cache mutable extracted
  dependency trees and that setup recreates a bounded external input root.
- Resolution: AppVeyor no longer caches mutable extracted source trees. Each
  release pass recreates its bounded input root and extracts only verified,
  pinned archives into empty destinations.
- Verification: Pipeline-isolation tests reject extracted-tree cache entries
  and non-empty or in-worktree release input roots.

### BUILD-027: Source provenance omits ignored and local build inputs

- Status: FIXED
- Code: `src/src.pro`, `src/Core/main.cpp`, and
  `src/Resources/linux/AppImagePackagingSupport.sh`
- Impact: A clean Git revision does not identify ignored `gcconfig.pri`,
  `GeneratedSecrets.h`, or arbitrary `LOCALHEADERS` and `LOCALSOURCES`.
  BUILD-011 metadata can therefore claim a committed source identity for a
  binary compiled from different inputs.
- Regression test: Change an ignored effective configuration after producing a
  binary provenance fixture and require packaging to reject it; reject local
  source/header injection in release input identity generation.
- Resolution: The compiled provenance and release manifest bind a deterministic
  identity for effective `gcconfig.pri`, optional `GeneratedSecrets.h`, and Qwt
  configuration. Release identity generation rejects local source and header
  extensions.
- Verification: Build-input fixtures mutate ignored configuration and require
  rejection, while compiled report and manifest identities must match exactly.

### BUILD-028: SBOM verification does not authenticate payload coverage

- Status: FIXED
- Code: `src/Resources/linux/generate-appimage-sbom.py` and
  `src/Resources/linux/AppImagePackagingSupport.sh`
- Impact: The verifier checks CycloneDX shape and sidecar equality but does not
  require one accurate component for every payload file and symlink. Missing,
  stale, or modified runtime files can pass the current gate.
- Regression test: Verify a complete fixture, then remove a component, alter a
  payload file, and redirect a symlink; every mutation must be rejected.
- Resolution: The SBOM preserves the primary payload role for every file and
  symlink and records Python provenance in a separate property. Verification
  enforces exact path coverage, hashes, sizes, modes, link targets, and payload
  containment.
- Verification: Complete payload fixtures pass; missing entries, altered files,
  redirected links, and Python-role substitutions are rejected.

### BUILD-029: AppImage tooling inherits release-affecting host variables

- Status: FIXED
- Code: `src/Resources/linux/AppImagePackagingSupport.sh`
- Impact: `appimagetool` and package smoke commands inherit variables such as
  `VERSION`, signing/update settings, Qt plugin paths, and Python paths. Host
  state can alter output or load code outside the verified package.
- Regression test: Poison every relevant variable in a fake packaging tool and
  require only the explicitly controlled locale, timezone, architecture, and
  source epoch to reach it.
- Resolution: Build, packaging, and smoke tools now run through separate
  `env -i` allowlists. The reproducible build path supplies only authenticated
  revision/epoch, fixed locale/timezone, private HOME/TMPDIR, Qt root, and
  deterministic archive settings; host compiler and flag variables are absent.
- Verification: Poisoned-environment tests confirm release-affecting host
  variables do not reach build or packaging tools.

### BUILD-030: Two-pass packaging reuses one compiled executable

- Status: OPEN
- Code: `appveyor/linux/after_build.sh`,
  `appveyor/linux/package-appimage-pass.sh`,
  `.devcontainer/package-appimage.sh`, and
  `src/Resources/linux/MakeAppImageQt6.sh`
- Impact: Byte-identical packages from the same ELF prove packaging
  determinism, not source-build reproducibility. Local and devcontainer paths
  additionally perform only one packaging pass.
- Regression test: Drive the orchestration with fake compilers that stamp
  distinct build roots, require two independent clean builds, and verify that
  each pass packages its own executable before comparison.
- Fix direction: Share one two-build/two-package orchestrator, isolate both
  build roots, and compare complete release outputs.

### BUILD-031: GUI smoke exits before GoldenCheetah runtime initialization

- Status: OPEN
- Code: `src/Core/main.cpp` and
  `src/Resources/linux/AppImagePackagingSupport.sh`
- Impact: The current marker proves only that `QApplication` entered its event
  loop. It exits before bundled keychain configuration, local-store process
  setup, and application defaults, so packaged startup regressions can pass.
- Regression test: Require the smoke marker path to occur after non-profile
  runtime initialization and to perform matching local-store shutdown.
- Fix direction: Keep the disposable profile, initialize the bounded runtime
  prerequisites, then emit an `application-runtime-ready` marker from the event
  loop and shut down cleanly.

### BUILD-032: APT snapshot integration bypasses the real Docker bootstrap

- Status: FIXED
- Code: `.devcontainer/Dockerfile` and
  `unittests/Build/appImagePackaging/testAptSnapshot.py`
- Impact: The optional integration test installs live CA and Python packages
  before switching to the snapshot, so it cannot validate the actual
  no-trust-yet bootstrap ordering used by the development image.
- Regression test: Build the Dockerfile's named bootstrap stage directly with
  no cache and require snapshot metadata verification and pinned CA/OpenSSL
  installation to complete there.
- Resolution: The real Docker bootstrap stage is built directly. Its initial
  trust store comes from a SHA-256-pinned CA package whose digest was verified
  against the signed Ubuntu snapshot index; APT then installs the exact pinned
  CA/OpenSSL packages and verifies snapshot metadata before other dependencies.
- Verification: All 13 APT tests pass, including a no-cache networked build of
  the actual `apt-snapshot-bootstrap` stage.

## Verification Baseline

The complete containerized release matrix through `MEM-024`, `THREAD-018`, and
`GUI-011` passes:

- 100 QtTest programs
- 1 AppImage packaging check
- 1 compile-only generated-SIP prerequisite
- 4,016 passed
- 0 failed or blacklisted
- 12 expected platform-only skips on Linux
- Qt 6.8.3 on Ubuntu 24.04
- complete Qt application build and a 10-second disposable-HOME offscreen
  event-loop smoke test

The check run also includes the AppImage packaging consistency check and builds
its compile-only SIP prerequisite. The registered matrix includes the 32-case
Strava OAuth policy suite. Production AppImages are packaged from committed
source only after this matrix, the predecessor remains available as the
rollback image, and the local sidecar records the packaged commit and SHA-256
without making repository documentation depend on local artifact state.

`DATA-011` additionally passes its 44-case integrity and 34-case
refresh/integration suites normally, under strict ASan/UBSan/LSan with leak
detection, and under ThreadSanitizer without suppressions, plus its MinGW
header-order check. The timing-sensitive credential enrollment coverage
recorded as `TEST-004` previously failed two of nine observations. It passed in
the previous and current complete matrices, bringing the record to two failures
and nine passes in eleven observations; it does not touch either code path.
`DATA-013` and `DATA-017` additionally pass their 16-pass removal program
normally, under strict ASan/UBSan/LSan with leak detection, and under
ThreadSanitizer without suppressions.
`DATA-012` additionally passes 98 focused cases under strict
ASan/UBSan/LSan with leak detection and ThreadSanitizer without suppressions,
plus its MinGW header-order check. `DUR-006` additionally passes its 29-case
focused suite under strict
ASan/UBSan/LSan with leak detection and ThreadSanitizer without suppressions,
plus 20 consecutive normal runs. `PARSE-008` additionally passes its 32-case
focused suite under both sanitizer configurations and its MinGW header-order
check. `PARSE-005`'s 126 focused tests and the related 13 RideFile ownership
tests retain their sanitizer evidence. Earlier fixed memory/thread findings
retain the focused sanitizer and TSAN evidence recorded in their entries.

`DATA-018` additionally passes its current 231-case removal suite under
ASan/UBSan and
TSAN, its 115-case atomic-save suite under strict ASan/UBSan/LSan and TSAN, and
the related 28-, 33-, 28-, 15-, 17-, 7-, and 3-case split, lifecycle,
plan-reader, and callback suites under both configurations. `TEST-007` records
the only LSan exception: two synchronously destructive Qt model test rows each
retain 128 bytes of Qt 6.8.3 bookkeeping while remaining ASan/UBSan- and
TSAN-clean. The changed Windows atomic-file branch passes its MinGW64 syntax
check.

`DATA-019` additionally passes its final 147-case removal suite normally. Its
41 focused journal security, ordering, lease, startup, and 28-point
crash/recovery cases pass under strict ASan/UBSan/LSan and ThreadSanitizer
without reports. The related Split and athlete/startup suites pass 31 and 116
cases. The production application links and its disposable-HOME offscreen smoke
test exits successfully. Native Windows crash durability remains `DUR-014` and
was not inferred from the Linux or historical MinGW checks.

This baseline is not evidence for any remaining OPEN finding. Each open item
still requires its listed RED regression before implementation. No whole-suite
fuzzer or production-scale profiler campaign has been completed.
