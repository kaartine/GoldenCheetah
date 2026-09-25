# Qt 6.8.3 WebEngine Memcheck exception: control and regeneration

`../qt-6.8.3-webengine.supp` may only contain library-internal records that this
pure-Qt program reproduces without any GoldenCheetah code (see
`DEVELOPMENT-WORKFLOW.md`, "Memcheck exceptions"). Regenerate and re-review it
after changing Qt, libc, any library pinned in `../qt-6.8.3-webengine.supp.buildids`,
or Valgrind.

## Build

With the same Qt as the application (here `/opt/Qt/6.8.3/gcc_64`):

```sh
g++ -std=c++17 -O1 -g -fPIC control.cpp -o webengine_control \
    -I$QT/include -I$QT/include/QtCore -I$QT/include/QtGui -I$QT/include/QtWidgets \
    -I$QT/include/QtNetwork -I$QT/include/QtWebEngineCore -I$QT/include/QtWebEngineWidgets \
    -L$QT/lib -lQt6WebEngineWidgets -lQt6WebEngineCore -lQt6Widgets -lQt6Gui -lQt6Core \
    -Wl,-rpath,$QT/lib
```

## Generate stanzas

Run each variant in its own Valgrind process with the runners' Memcheck flags
(including `--keep-debuginfo=yes`) plus
`--gen-suppressions=all --xml=yes --xml-file=<variant>-<platform>.xml`,
`QT_ENABLE_REGEXP_JIT=0`, `QTWEBENGINE_DISABLE_SANDBOX=1` and
`LIBGL_ALWAYS_SOFTWARE=1`, on the default renderer:

- offscreen, `/opt/Qt` and system libraries (QtTest fixtures): `baseline`,
  `default`, `disk-noloop`, `gc-like`, `conn-alive`, `conn-hidden`,
  `conn-deleted`, and `qcss_control layout gc-css.html` (build `qcss_control.cpp`
  the same way; `gc-css.html` is the style string `IntervalSummaryWindow`
  passes to `QTextEdit::setHtml`);
- xcb under Xvfb with the gated AppDir's libraries (`LD_LIBRARY_PATH=<AppDir>/lib`,
  `QT_PLUGIN_PATH=<AppDir>/plugins`), so stanzas through its bundled glib are
  verified against that exact build: `baseline`, `default`, `disk-exec`,
  `view-sethtml`, `gc-like`, `gc-like-alive`, `conn-alive`, `conn-hidden`,
  `pixmap-icon <png>` and `pixmap-formats <png>` (the application's
  `src/Resources/sidebar/athlete.png` as input data), `gc-startup-pe` three times,
  and `a11y-label` inside `dbus-run-session -- sh atspi-run.sh` (the session bus
  and AT-SPI services with the system libraries, as in the UI runner).

`baseline` must stay free of blocking records offscreen; on xcb its only
blocking record is the libxcb `writev` syscall-parameter record from
`QXcbConnection` screen initialisation (2026-09-25).

`conn-alive` only justifies connection records of objects that are still alive
at exit: in the gated report the same GoldenCheetah constructors must have no
definitely or indirectly lost records, otherwise the objects leak and it is a
GoldenCheetah finding.

Then generate the files from all control reports and the unsuppressed gate and
fixture reports of the runtime being gated:

```sh
python3 final_supp.py OUT <AppDir glib Build-ID> ../qt-6.8.3-webengine.supp.buildids \
    <unsuppressed gate and fixture memcheck-*.xml> \
    -- <offscreen control XMLs (system libraries)> \
    -- <xcb control XMLs (AppDir libraries)>
```

`gen_supp.py` cuts each stanza at the first frame of the program itself, so no
stanza can contain a GoldenCheetah, test or control frame. It also emits shorter
stanzas cut inside the libraries, when the allocating or erroring frame is library
code and at least 7 non-allocator frames precede the cut: where the stack first
enters a different Qt library, or directly after
`QEventDispatcherGlib::processEvents` (the program's event-loop driver is above
it). Frames outside any object (e.g. JIT code) make a record unpinnable; it is
rejected. `final_supp.py` then

- drops glib stanzas (a `libglib` object or a `g_*` function frame) from system-glib controls (the AppImage bundles a different
  glib build) and puts the AppDir-glib ones in `qt-6.8.3-webengine-appdir-glib.supp`;
- accepts error records (invalid access, uninitialised value, syscall parameter)
  only with the control's full chain (`program` or `complete`), rejects leak
  stanzas cut inside the libraries whose frames are all unnamed objects unless
  they match only indirect leaks, and names every stanza
  `<n>-<kind>-<program|complete|dispatcher|library|truncated>-<named|objonly>`
  (a chain without a program frame is `complete` only if it ends at a thread or
  process root, otherwise `truncated`, handled like a cut inside the libraries);
- keeps a stanza only if it matches a record of the unsuppressed reports
  (offline, like `ledger.py`) or Valgrind credited it in a run with the candidate
  files (pass those as `<candidate file>:<report.xml>`), lists the records no
  stanza matches, and fails on an unpinned soname or an object wildcard.

Pin every contributing soname's Build-ID in the `.buildids` sidecars, including Qt
plugins such as `libqxcb.so` and Mesa's `swrast_dri.so`; the runners resolve
plugins through `QT_PLUGIN_PATH` and the Qt installation's `plugins` directory,
and DRI drivers through `LIBGL_DRIVERS_PATH` and the system `dri` directory.
Prepend the headers of the current files.

## Verify

Run the gated scenario with and without the files on the same binary and check
the pair with `python3 ledger.py <files> LABEL:<nosupp.xml>:<supp.xml>`: every
removed record must correspond to a control-verified stanza, and no removed record
may have its allocation or error start in GoldenCheetah code. For every gate and
fixture run, also run `python3 check_widened.py ../qt-6.8.3-webengine.supp <nosupp.xml>`: the two
definite,indirect stanzas may match only single blocks of the control sizes.
Keep both reports.
