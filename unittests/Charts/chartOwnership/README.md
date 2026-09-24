# Optional production chart ownership regression

This QtTest links the actual application's objects and runs production
constructors/destructors without entering the application's `main()` or loading
an athlete. It is intentionally opt-in: it requires a compatible, complete native
application build, including its resources, generated moc objects and libraries.
It adds no dependency to the normal lightweight unit-test inventory.
The provided make fragment targets **Linux with native Qt 6** specifically
(`libQt6Test.so`); it is not a generic cross-platform full-build test target.

Coverage:

- `AllPlot`: its real `CurveColors` helper is destroyed (a `QPointer` clears).
- `AllPlotObject`, through `AllPlot`: both production-created interval curves
  destroy their owned `QwtSeriesData`. Counting series are observation probes,
  not substitute chart implementations. A derived accessor only exposes the
  protected owner; its constructor/destructor remain the production ones.
- `AllPlotObject`: both attached selection markers are destroyed. The test
  assigns its counting markers before reading these formerly uninitialized fields.
- `GcChartWindow`: both animations and its timer are direct children, and their
  `QPointer`s clear when the actual chart is destroyed.
- `FilterEditor`: its completer/model are owned and reused, and repeated filter
  setup does not duplicate update connections.

The same make fragment also accepts `CHART_FIXTURE_TEST_NAME` to select a
separate production-object fixture; give each build its own output directory.

| Test source stem | Additional ownership coverage |
|---|---|
| `testWidgetOwnership` | RideEditor model/delegate, Cloud animation, TagBar completion model |
| `testViewOwnership` | Overview actions and ChartSpace animations |
| `testOverviewConfigOwnership` | Real RPE config lifetime, dialog close/destruction/remove, external tile removal |
| `testLibraryOwnership` | Shared Library initialization, final cleanup, idempotence, reinitialization and parsed libraries |

Overview's RPE fixture does not by itself verify all other tile subclasses or
shown/export dialogs. The widget fixture creates an isolated synthetic TrainDB;
none of these fixtures loads an athlete. Library cleanup must stay at final
application exit, not per-window or per-restart, because the registry is shared.

The charts remain unshown. There is no ride, athlete, chart window for interval
selection, rendering exercise or UI shutdown simulation. These are ownership
assertions, **not a claim that the whole application is Memcheck-clean**. Retain
the separate full-application graceful-shutdown Memcheck gate.

## Build

Use the compiler/runtime environment of the supplied native, non-ASan build.
The source and cached objects must match; this fixture cannot verify that a
cache's objects are current. Mount the source and cache read-only when practical.
From the directory containing the application's generated `src/Makefile`:

```sh
mkdir /absolute/new-chart-fixture
make -rR -f Makefile \
  -f /absolute/source/unittests/Charts/chartOwnership/chartOwnership.mk \
  -o Makefile CHART_FIXTURE_OUT=/absolute/new-chart-fixture \
  CHART_FIXTURE_MAIN=/absolute/source/src/Core/main.cpp chart-ownership
```

For another fixture, add for example
`CHART_FIXTURE_TEST_NAME=testOverviewConfigOwnership` to that make command.
The default output binary name remains `tst_chartOwnership`; override
`CHART_FIXTURE_BINARY` if desired.

The fragment uses the cached `CXXFLAGS`, `INCPATH`, `OBJECTS`, `LIBS`, `QMAKE`
and link flags. It does not invoke application-object build rules. It compiles
`main.cpp` into the output directory with just its entry point renamed, retaining
production global definitions, and substitutes that object for cached `main.o`.
Use paths without whitespace, as required by this make fragment's compiler
arguments. Match all compile-time application features to the cached build.

`CHART_FIXTURE_LIBS` can explicitly override the link libraries when reproducing
a known diagnostic environment. Do not silently substitute ABI-incompatible
libraries. For the reviewed Jammy diagnostic cache, excluding the extracted host
GSL search directory selects the image's compatible GSL libraries.

## Isolated native run

Production static initialization occurs before QtTest starts, including creation
of the settings singleton. Run in a disposable account or network-disabled
container without mounting real user settings or athlete data. The launcher
sets fresh private XDG/temp paths **before** execution and refuses existing
output directories; the in-test environment check is only an additional guard.

```sh
sh /absolute/source/unittests/Charts/chartOwnership/run-native.sh \
  /absolute/new-chart-fixture/tst_chartOwnership \
  /absolute/new-chart-fixture/native
```

The exit status is QtTest's status. `qttest.xml`, `qttest.txt` and
`application.log` remain in the new run directory. Optional trailing arguments
select QtTest functions. Supply the build's Qt/Qwt runtime library paths as
needed. Offscreen graphics/WebEngine initialization warnings can occur even
though the fixture never opens a chart or athlete.

For a separately requested Memcheck run, use `.github/scripts/run-memcheck.py`
inside that same isolated runtime and set `GC_CHART_OWNERSHIP_ROOT` to the
runner's absolute new `--output` directory. The runner creates its matching XDG
paths. Do not nest `run-native.sh` under Valgrind: instrument the native binary.

## Differential old-object check

Keep fixed and old executables separate. After building the fixture, the optional
`chart-ownership-link` target reuses its generated test and renamed-main objects.
Set `CHART_FIXTURE_BINARY` to a different output filename, and explicitly override
`CHART_FIXTURE_OBJECTS` with the normal list minus the selected fixed objects,
plus ABI-compatible preserved pre-fix objects. For example:

```sh
make -rR -f Makefile \
  -f /absolute/source/unittests/Charts/chartOwnership/chartOwnership.mk \
  -o Makefile CHART_FIXTURE_OUT=/absolute/new-chart-fixture \
  CHART_FIXTURE_BINARY=/absolute/new-chart-fixture/tst_chartOwnership-old \
  'CHART_FIXTURE_OBJECTS=$(filter-out main.o AllPlot.o GoldenCheetah.o,$(OBJECTS)) /absolute/baseline/AllPlot.o /absolute/baseline/GoldenCheetah.o' \
  chart-ownership-link
```

Run it with another fresh run directory. This demonstrates regression sensitivity
without reverting shared source or overwriting any cached production object.
