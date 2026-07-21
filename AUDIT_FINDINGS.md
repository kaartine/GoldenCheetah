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
- Code: `src/Core/GcUpgrade.h:142`, `src/Gui/AbstractView.cpp:352`,
  `src/Gui/AbstractView.cpp:1016`, `src/Gui/AbstractView.cpp:1037`,
  `src/Charts/PythonChart.cpp:578`
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
  not exist. Its five QtTest cases pass, the application builds, and the full
  suite passes 81 tests with zero failures.

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

## Medium

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

- Status: OPEN
- Code: `src/FileIO/CsvRideFile.cpp:167`,
  `src/FileIO/CsvRideFile.cpp:1103`
- Impact: One attacker-controlled timestamp can consume CPU and memory for a
  billion-iteration expansion.
- Test: Import huge, negative, non-finite, and non-monotonic timestamps.
- Fix direction: Global point/duration budgets and no per-second materialization.

### PARSE-003: TCX swim gaps amplify into thousands of points per record

- Status: OPEN
- Code: `src/FileIO/TcxParser.cpp:214`,
  `src/FileIO/TcxParser.cpp:314`, `src/FileIO/TcxParser.cpp:380`
- Impact: Crafted trackpoints can cause large memory/CPU amplification.
- Test: Exercise repeated maximum gaps and enforce a whole-file point budget.
- Fix direction: Avoid synthetic per-second allocation and bound whole-file work.

### PARSE-004: Malformed XML is accepted as a partial activity

- Status: OPEN
- Code: `src/FileIO/TcxRideFile.cpp:58`,
  `src/FileIO/GpxRideFile.cpp:44`, `src/FileIO/FitlogRideFile.cpp:49`
- Impact: Parser callbacks mutate a ride before a malformed tail is ignored.
- Test: Truncate each XML format at every structural boundary and require reject.
- Fix direction: Check parse results/error handlers and discard partial state.

### PARSE-005: JSON parser checks the wrong error list

- Status: OPEN
- Code: `src/FileIO/JsonRideFile.y:80`,
  `src/FileIO/JsonRideFile.y:445`, `src/FileIO/JsonRideFile.y:452`
- Impact: Malformed JSON can return a partially populated activity as valid.
- Test: Syntax errors before/after each major section must reject the ride.
- Fix direction: Check parser return status and the actual parser error list.

### PARSE-006: FIT integrity and truncation checks are incomplete

- Status: OPEN
- Code: `src/FileIO/FitRideFile.cpp:4054`,
  `src/FileIO/FitRideFile.cpp:4074`, `src/FileIO/FitRideFile.cpp:4693`,
  `src/FileIO/FitRideFile.cpp:4710`
- Impact: Length narrowing, ignored CRCs, and partial recovery can accept corrupt
  files and inconsistent data.
- Test: Wrong lengths, header/data CRCs, and truncation must fail by default.
- Fix direction: Checked unsigned arithmetic, physical-size limits, CRC checks,
  and explicit opt-in partial recovery.

### DUR-005: QSettings migration is not resumable after partial success

- Status: OPEN
- Code: `src/Core/Settings.cpp:384`, `src/Core/Settings.cpp:213`
- Impact: Any partially populated new settings file suppresses remaining legacy
  migration, making configuration and credentials appear lost.
- Test: Fail each migration write/sync point and restart migration.
- Fix direction: Idempotent per-key migration plus a completion marker written
  only after all `sync()`/`status()` checks succeed.

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

## Low

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

## Verification Baseline

The complete containerized release matrix after SEC-011 passes:

- 52 QtTest suites
- 1,930 passed
- 0 failed, skipped, or blacklisted
- Qt 6.8.3 on Ubuntu 24.04

SEC-011's 69 focused tests also pass under strict ASan/UBSan/LSan with leak
detection, and the packaged AppImage passes isolated startup and two-process
migration-failure usage tests. Earlier fixed memory/thread findings retain the
focused sanitizer and TSAN evidence recorded in their entries.

This baseline is not evidence for any remaining OPEN finding. Each open item
still requires its listed RED regression before implementation. No whole-suite
fuzzer or production-scale profiler campaign has been completed.
