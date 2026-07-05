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

- Status: OPEN
- Code: `src/Train/BT40Device.cpp:421`
- Impact: A function-static `VMProWidget` is parented to the first BLE device.
  Destroying the device frees the widget but leaves the static pointer, so the
  next reconnect calls through freed memory. Multiple VO2 devices also share it.
- Test: Connect, stop, recreate the controller, and reconnect under ASan. Also
  verify two VO2 devices retain independent configurators.
- Fix direction: Store a per-device `QPointer<VMProWidget>` member.

## High

### DUR-001: Activity saves are non-atomic and report false success

- Status: OPEN
- Code: `src/FileIO/JsonRideFile.y:784`, `src/Gui/SaveDialogs.cpp:189`,
  `src/Core/RideCache.cpp:1771`
- Impact: JSON save truncates the destination in place, does not check write or
  flush status, and callers mark the activity clean even after failure. Disk
  exhaustion or a crash can destroy the only good copy.
- Test: Inject open, short-write, flush, and commit failures. The original bytes
  and dirty state must remain intact.
- Fix direction: Use `QSaveFile`, check every result, propagate errors, and only
  mark clean after a successful atomic commit and readback where appropriate.

### DUR-002: Other persistent files are also truncated in place

- Status: OPEN
- Code: `src/Core/Measures.cpp:152`, `src/Core/Seasons.cpp:224`,
  `src/Metrics/RideMetadata.cpp:1671`, `src/Core/RideDB.y:481`
- Impact: Measures, seasons, metadata, and cache state can be left empty or
  partial on ENOSPC or process failure.
- Test: Add fault-injection tests for each writer and preserve the prior file.
- Fix direction: Share the atomic persistence helper introduced for DUR-001.

### DUR-003: TrainDB drops user tables when version lookup fails

- Status: OPEN
- Code: `src/Train/TrainDB.cpp:725`
- Impact: A version-table read error is treated as an obsolete schema and can
  drop tags, ratings, last-run state, and other user-maintained tables without a
  transaction or backup.
- Test: Open locked, corrupt, missing-version, and old-version databases and
  verify read errors never mutate data.
- Fix direction: Distinguish query failure from an explicit old schema and use
  transactional, versioned migrations with backup/rollback.

### DUR-004: Full athlete backup is incomplete and not verified

- Status: OPEN
- Code: `src/FileIO/AthleteBackup.cpp:40`,
  `src/FileIO/AthleteBackup.cpp:178`, `src/Train/TrainDB.cpp:174`
- Impact: Planned activities, root-level database state, nested files, and read
  failures may be omitted while backup still reports success. Media is loaded
  fully into memory.
- Test: Compare a fixture athlete tree against the archive manifest and force
  read/write failures.
- Fix direction: Define a persistent-data manifest, stream recursively, check
  writer status, and verify the archive before atomic publication.

### TRN-001: Device errors automatically delete the current recording

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:1607`,
  `src/Train/TrainSidebar.cpp:1699`,
  `src/Train/ComputrainerController.cpp:85`
- Impact: A late trainer disconnect maps directly to `DiscardRecording`,
  deleting otherwise valid workout data without confirmation.
- Test: Record samples, call `Stop(DEVICE_ERROR)`, and verify the raw recording
  remains recoverable until the user explicitly discards it.
- Fix direction: Stop hardware independently from recording disposition and
  default errors to preserving/recovering partial data.

### TRN-002: Recording I/O failures are silent

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:1466`,
  `src/Train/TrainSidebar.cpp:1548`, `src/Train/TrainSidebar.cpp:2362`
- Impact: Failure to create, write, or flush the workout CSV is not surfaced.
  Training continues while the UI implies that data is being recorded.
- Test: Use an unwritable path and a failing/short-write device and verify a
  visible fatal recording state plus preservation of any partial data.
- Fix direction: Model recording health explicitly and check every I/O result.

### TRN-003: Auxiliary telemetry timestamps become non-monotonic across pause

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:1331`,
  `src/Train/TrainSidebar.cpp:3422`, `src/Train/TrainSidebar.cpp:3491`
- Impact: RR, position, core-temperature, and VO2 samples continue during pause
  using a timer that is reset on resume. Timestamps can then jump backwards.
- Test: Pause, emit each auxiliary sample, resume, and require no paused samples
  plus strictly increasing active-session timestamps.
- Fix direction: Use one monotonic active-session clock and gate side channels
  while paused or calibrating.

### DEV-001: Failed devices are still reported as connected

- Status: OPEN
- Code: `src/Train/TrainSidebar.cpp:1850`,
  `src/Train/TrainSidebar.cpp:1854`, `src/Train/TrainSidebar.cpp:1612`
- Impact: Controller start results are ignored and polling begins regardless.
  Failed devices can remain in a connected UI state and emit repeated errors.
- Test: Use a fake controller whose start fails and assert transactional rollback,
  inactive timers, and a disconnected state.
- Fix direction: Make multi-device connection an all-or-reported-partial
  transaction and roll back already-started controllers on failure.

### BLE-002: FTMS target scaling can divide by zero

- Status: OPEN
- Code: `src/Train/BT40Device.cpp:537`,
  `src/Train/BT40Device.cpp:1364`, `src/Train/Ftms.cpp:80`
- Impact: FTMS load control is enabled before asynchronous range discovery has
  supplied a positive increment. A delayed or missing response reaches division
  by zero and sends invalid targets.
- Test: Simulate delayed, absent, zero, and malformed range characteristics.
- Fix direction: Track capability/range readiness and queue the latest target
  until validated limits are available.

### BLE-003: Multiple BLE sources overwrite one shared telemetry object

- Status: OPEN
- Code: `src/Train/AddDeviceWizard.cpp:1005`,
  `src/Train/BT40Controller.h:67`, `src/Train/BT40Device.cpp:202`
- Impact: A trainer, HR belt, and power meter race by notification timing.
  Disconnecting one source also clears values still supplied by another source.
- Test: Interleave two sources for the same metric and disconnect either one.
- Fix direction: Maintain per-device snapshots and explicit per-metric source
  ownership, priority, and staleness.

### BLE-004: BLE service discovery initializes services repeatedly

- Status: OPEN
- Code: `src/Train/BT40Device.cpp:371`
- Impact: Each service completion loops over every discovered service, repeating
  CCCD writes, FTMS control requests, Wahoo queue resets, and VO2 setup.
- Test: Complete two or more services in different orders and assert one setup
  operation per characteristic.
- Fix direction: Process only `sender()` and record per-service initialization.

### DEV-002: ANT workers are not stopped and joined before destruction

- Status: OPEN
- Code: `src/ANT/ANTlocalController.cpp:41`, `src/ANT/ANT.cpp:641`,
  `src/Train/TrainSidebar.cpp:730`
- Impact: Workers can retain USB resources, race controller destruction, leak
  from the pairing wizard, or trigger `QThread destroyed while running`.
- Test: Repeatedly start/stop/delete a fake blocking transport and require zero
  surviving workers and immediate port reuse.
- Fix direction: Give controllers worker ownership, unblock I/O, request stop,
  and synchronously `wait()` in teardown.

### DEV-003: ANT telemetry and command queues have data races

- Status: OPEN
- Code: `src/ANT/ANT.cpp:320`, `src/ANT/ANT.h:486`,
  `src/ANT/ANT.cpp:687`
- Impact: GUI and worker threads concurrently mutate queues and telemetry without
  a common lock, producing undefined behavior.
- Test: Stress channel changes and telemetry polling under TSAN.
- Fix direction: Keep transport state worker-owned and publish immutable or
  mutex-protected telemetry snapshots.

### DEV-004: Stale ANT/BLE telemetry can be recorded indefinitely

- Status: OPEN
- Code: `src/ANT/ANTChannel.cpp:197`, `src/ANT/ANT.cpp:904`,
  `src/Train/BT40Device.cpp:207`
- Impact: Silent sensors retain their last values; some disconnect paths do not
  clear cadence. Recordings can contain plausible but stale data.
- Test: Stop notifications with a fake clock and verify metric-specific expiry.
- Fix direction: Timestamp every metric per source and expire it using protocol
  appropriate timeouts.

### THREAD-001: Cloud auto-download can outlive its athlete/context

- Status: OPEN
- Code: `src/Core/Athlete.cpp:169`, `src/Core/Athlete.cpp:238`,
  `src/Cloud/CloudService.cpp:1805`
- Impact: Closing an athlete during download leaves an unowned worker that can
  dereference the destroyed athlete through a still-registered context.
- Test: Block a fake provider mid-download, close the athlete, and run under
  ASan/TSan.
- Fix direction: Athlete teardown must cancel, join, and delete the worker before
  dependent state is destroyed.

### THREAD-002: Cloud download state is mutated from GUI and worker threads

- Status: OPEN
- Code: `src/Cloud/CloudService.cpp:1784`,
  `src/Cloud/CloudService.cpp:1882`, `src/Cloud/CloudService.cpp:1924`
- Impact: A GUI-affine QThread object and its worker path concurrently iterate,
  clear, and delete providers and download entries.
- Test: Exercise immediate, delayed, and timeout completions under TSAN.
- Fix direction: Move a separate worker QObject to the thread and pass immutable
  results to the GUI.

### MEM-004: PythonDataSeries copy is a double-free/use-after-free

- Status: OPEN
- Code: `src/Python/SIP/Bindings.h:14`,
  `src/Python/SIP/Bindings.cpp:797`,
  `src/Python/SIP/sipgoldencheetahPythonDataSeries.cpp:299`
- Impact: Generated copying shallow-copies the owning `double*`; both wrappers
  later call `delete[]`.
- Test: Copy a series, destroy the source, access and destroy the copy under ASan.
- Fix direction: Store data in a value container or implement/delete copy and
  move operations explicitly.

### MEM-005: PowerTap line reader can overflow its stack buffer

- Status: OPEN
- Code: `src/FileIO/PowerTapDevice.cpp:48`,
  `src/FileIO/PowerTapDevice.cpp:114`, `src/FileIO/PowerTapDevice.cpp:142`
- Impact: The only capacity guard is `assert`, absent in release builds. A device
  that omits CRLF writes beyond the 256-byte version buffer, while newline scan
  also inspects one byte beyond valid input.
- Test: Use a fake CommPort returning 256+ non-newline bytes and a trailing CR.
- Fix direction: Enforce capacity before every read and scan only while
  `i + 1 < length`.

### MEM-006: CAF parser relies on release-disabled bounds assertions

- Status: OPEN
- Code: `src/FileIO/TacxCafRideFile.cpp:132`,
  `src/FileIO/TacxCafRideFile.cpp:154`,
  `src/FileIO/TacxCafRideFile.cpp:253`
- Impact: Truncated blocks and zero-record blocks cause out-of-bounds reads once
  `Q_ASSERT` is compiled out.
- Test: Fuzz truncation at each byte boundary and zero-record blocks under ASan.
- Fix direction: Validate block headers, extents, products, and record counts in
  release code.

### MEM-007: TTS handlers accept empty/short blocks and unsafe typed reads

- Status: OPEN
- Code: `src/FileIO/TTSReader.cpp:528`,
  `src/FileIO/TTSReader.cpp:1042`, `src/FileIO/TTSReader.cpp:1141`
- Impact: Empty blocks can be read at negative offsets and short blocks access
  missing bytes. Typed pointer loads are also unaligned/aliasing unsafe.
- Test: Exercise empty and 1-15 byte blocks under ASan/UBSan.
- Fix direction: Add per-block size contracts and decode with endian helpers.

### MEM-008: Custom virtual trainer names use mismatched new[]/delete

- Status: OPEN
- Code: `src/Train/RealtimeController.cpp:716`,
  `src/Train/AddDeviceWizard.cpp:1275`,
  `src/Train/RealtimeController.cpp:745`
- Impact: Destroying a custom trainer invokes undefined allocator behavior.
- Test: Add trainers through both paths and repeatedly destroy under ASan.
- Fix direction: Store the name as `QString`/`std::string`, or minimally use
  `delete[]`.

### MEM-009: Cancelled migration leaves Athlete owning pointers indeterminate

- Status: OPEN
- Code: `src/Core/GcUpgrade.cpp:389`, `src/Core/Athlete.cpp:92`,
  `src/Core/Athlete.cpp:241`, `src/Core/Athlete.h:96`
- Impact: Constructor return after cancellation precedes member initialization,
  but the destructor later deletes those members.
- Test: Cancel migration and destroy the athlete under ASan.
- Fix direction: Initialize every owner to null and propagate construction
  failure instead of returning a partially constructed logical object.

### THREAD-003: Python chart execution races GUI object lifetime

- Status: OPEN
- Code: `src/Charts/PythonChart.cpp:578`,
  `src/Charts/PythonChart.cpp:620`, `src/Charts/PythonChart.cpp:626`
- Impact: Worker code dereferences GUI objects and a raw chart pointer while a
  nested GUI event loop permits edits and deletion.
- Test: Run a sleeping script while editing and deleting the chart under
  ASan/TSan.
- Fix direction: Snapshot value data on the GUI thread and guard completion with
  owned task state or `QPointer`.

### SEC-004: OpenData discovery can redirect the full dataset

- Status: OPEN (feature is disabled in the current local build)
- Code: `src/Cloud/OpenData.h:33`, `src/Cloud/OpenData.cpp:175`,
  `src/Cloud/OpenData.cpp:317`
- Impact: An HTTP-discovered arbitrary URL receives the opted-in athlete UUID
  and activity dataset. A network attacker can redirect uploads.
- Test: Supply a tampered discovery response and verify no request or upload is
  sent outside a signed HTTPS allowlist.
- Fix direction: Signed HTTPS discovery, strict host allowlist, and redirect
  validation.

### SEC-005: Local HTTP API has no authentication or Host validation

- Status: OPEN (server is disabled by default)
- Code: `src/Core/main.cpp:647`, `src/Core/APIWebService.cpp:38`,
  `contrib/httpserver/httplistener.cpp:36`
- Impact: When enabled, DNS rebinding can expose demographics, measures, zones,
  activities, and GPS data to a malicious website.
- Test: Reject attacker Host/Origin values and missing bearer tokens while
  allowing authenticated loopback clients.
- Fix direction: Mandatory random bearer token, loopback-only binding, and
  strict Host/Origin checks.

### SEC-006: Legacy OAuth callbacks are not bound to the initiating session

- Status: OPEN
- Code: `src/Cloud/OAuthDialog.cpp:99`,
  `src/Cloud/OAuthDialog.cpp:142`, `src/Cloud/OAuthDialog.cpp:221`,
  `src/Cloud/OAuthDialog.cpp:395`
- Impact: HTTP callbacks, absent/fixed state, broad URL matching, and accepting
  TLS-handshake failure allow code interception or account-binding CSRF.
- Test: Reject wrong state, host, path, callback replay, HTTP callbacks, and TLS
  failures without changing stored credentials.
- Fix direction: Migrate all providers to system-browser loopback PKCE with
  random one-time state and exact parsed callback matching.

### SEC-007: Active legacy providers send credentials and health data over HTTP

- Status: OPEN
- Code: `src/Cloud/SportsPlusHealth.cpp:55`,
  `src/Cloud/SportsPlusHealth.cpp:136`,
  `src/Cloud/TrainingsTageBuch.cpp:53`,
  `src/Cloud/TrainingsTageBuch.cpp:93`
- Impact: Network observers can capture credentials and full activity uploads.
- Test: Ensure HTTP endpoints fail before any credential or payload is sent.
- Fix direction: Require verified HTTPS or disable obsolete integrations.

### SEC-008: Remote WebEngine downloads are automatically imported

- Status: OPEN
- Code: `src/Train/WebPageWindow.cpp:148`,
  `src/Train/WebPageWindow.cpp:267`, `src/Train/WebPageWindow.cpp:283`
- Impact: A malicious page using the shared profile can silently drive files
  into complex ride/workout parsers without a trusted origin or user gesture.
- Test: Trigger downloads from another shared-profile page and script; require
  explicit confirmation before parsing.
- Fix direction: Isolate profiles, validate page/origin, quarantine downloads,
  and require user approval.

### SEC-009: Map WebChannel is exposed to insecure/untrusted scripts

- Status: OPEN
- Code: `src/Charts/RideMapWindow.cpp:161`,
  `src/Charts/RideMapWindow.cpp:613`, `src/Charts/RideMapWindow.h:81`
- Impact: Google Maps JavaScript is loaded over HTTP in a page exposing route
  coordinates and interval mutation through WebChannel.
- Test: Assert no HTTP subresource loads and untrusted pages cannot resolve or
  invoke the bridge.
- Fix direction: Bundle or require HTTPS scripts and isolate third-party code
  from the privileged bridge.

### SEC-010: Interval names are inserted into JavaScript without escaping

- Status: OPEN
- Code: `src/Charts/RideMapWindow.cpp:1543`
- Impact: An imported activity can provide an interval name that breaks out of a
  JavaScript string, reads route data through WebChannel, and sends it remotely.
- Test: Render interval names containing quotes, newlines, and script payloads;
  verify they remain inert text.
- Fix direction: Pass structured values through WebChannel/JSON rather than
  source-code string construction.

### SEC-011: Cloud credentials are stored in plaintext settings

- Status: OPEN
- Code: `src/Cloud/CloudService.h:549`, `src/Core/Settings.cpp:432`,
  `src/Core/Settings.h:386`
- Impact: Backups or local processes able to read athlete settings obtain
  reusable access and refresh tokens. File permissions are not hardened in code.
- Test: Save/migrate a sentinel secret and verify it appears only in a mocked OS
  credential vault, never in INI files.
- Fix direction: OS keychain integration with migration; enforce 0700/0600 as an
  interim defense.

### SEC-012: Secrets are logged or placed in URLs

- Status: OPEN
- Code: `src/Cloud/Azum.cpp:178`, `src/Cloud/Azum.cpp:310`,
  `src/Cloud/OAuthDialog.cpp:307`, `src/Cloud/WithingsDownload.cpp:130`
- Impact: Tokens/passwords can enter terminals, journald, crash reports, proxy
  logs, history, or support bundles.
- Test: Capture Qt messages and network requests using sentinel credentials and
  assert no secret occurs in logs or URLs.
- Fix direction: Remove token logging, centralize redaction, and use
  Authorization headers/body parameters.

### PERF-001: Merge activity alignment is O(series * samples^2) on the UI thread

- Status: OPEN
- Code: `src/Gui/MergeActivityWizard.cpp:191`,
  `src/Gui/MergeActivityWizard.cpp:1077`
- Impact: Multi-hour activities can execute hundreds of millions of iterations
  while freezing the UI.
- Test: Benchmark 1h and 3h fixtures and enforce a bounded UI stall.
- Fix direction: Coarse-to-fine or FFT correlation in a cancellable worker.

### PERF-002: Bulk import parses files repeatedly and rebuilds global state

- Status: OPEN
- Code: `src/Gui/RideImportWizard.cpp:583`,
  `src/Gui/RideImportWizard.cpp:1119`, `src/Core/RideCache.cpp:315`
- Impact: Files are parsed in validation and again during save; each addition can
  reset/sort the full library on the UI thread.
- Test: Benchmark 100/1000 FIT imports into a 10k-activity fixture.
- Fix direction: Parse once in workers and perform a single bulk model merge.

### PERF-003: RideCache blocks startup and repeatedly scans the full library

- Status: OPEN
- Code: `src/Core/RideCache.cpp:111`, `src/Core/Athlete.cpp:180`,
  `src/Core/RideCache.cpp:708`
- Impact: Large libraries delay time-to-interactive and configuration changes.
- Test: Measure cold/warm startup and invalidation at 1k/10k/50k activities.
- Fix direction: Incremental background loading, generation-based cancellation,
  and dependency-specific invalidation.

### PERF-004: DataFilter leaks models and its GSL RNG

- Status: OPEN
- Code: `src/Core/DataFilter.cpp:3283`, `src/Core/DataFilter.h:222`
- Impact: Frequently recreated filters retain five model objects plus an RNG,
  increasing memory use during navigation.
- Test: Create/destroy 10,000 filters under heaptrack/ASan leak detection.
- Fix direction: RAII ownership, `qDeleteAll(rt.models)`, and `gsl_rng_free`.

### PERF-005: Navigator filtering is quadratic for large result sets

- Status: OPEN
- Code: `src/Gui/RideNavigatorProxy.h:740`,
  `src/Gui/RideNavigatorProxy.h:765`
- Impact: `filterAcceptsRow` performs a linear `QStringList::contains` lookup for
  every source row.
- Test: Filter 50k rows with 25k matches and enforce a latency budget.
- Fix direction: Store matches in `QSet<QString>`.

### PERF-006: Calendar/compare aggregation repeatedly scans the library

- Status: OPEN
- Code: `src/Core/RideCache.cpp:784`,
  `src/Charts/CalendarWindow.cpp:1145`
- Impact: Each metric and time bucket triggers another full activity scan.
- Test: Benchmark calendar/compare refresh at 1k/10k/50k activities.
- Fix direction: Batch metrics/buckets in one scan or maintain incremental daily
  aggregates.

## Medium

### PARSE-001: ZIP/GZIP decompression has no resource limits

- Status: OPEN
- Code: `contrib/qzip/zip.cpp:874`, `contrib/qzip/zip.cpp:902`,
  `src/FileIO/RideFile.cpp:839`, `src/FileIO/RideFile.cpp:892`
- Impact: Compression bombs can exhaust memory or freeze the UI.
- Test: Enforce rejection of excessive entry count, entry size, total output, and
  compression ratio.
- Fix direction: Bounded streaming to temporary files with CRC/size validation.

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

### BUILD-001: Release dependencies and tooling are not reproducibly pinned

- Status: OPEN
- Code: `appveyor/linux/after_build.sh:36`,
  `appveyor/linux/install.sh:28`, `src/Python/requirements.txt:5`
- Impact: Moving tool/dependency targets can change or break artifacts for the
  same source commit.
- Test: Repeat the build from a locked manifest and compare dependency/SBOM data.
- Fix direction: Pin commits/digests, hash-lock Python dependencies, generate an
  SBOM, and smoke-test AppImage on the oldest supported glibc.

## Verification Baseline

The existing containerized unit suite passed at audit time:

- 76 passed
- 0 failed
- Qt 6.8.3, Ubuntu 24.04 container

The suite currently has no direct coverage for the critical archive, parser,
cloud, device-protocol, persistence, or TrainSidebar state-machine findings.
No sanitizer, fuzzer, or production-scale profiler run has yet been completed.
