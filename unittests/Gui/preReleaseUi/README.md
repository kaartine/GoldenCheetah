# Pre-release UI tests

This suite exercises a packaged Linux AppImage through Qt's AT-SPI
accessibility interface. It creates a temporary athlete library and never
opens the user's normal GoldenCheetah data.

Run it with:

```bash
unittests/Gui/preReleaseUi/run-pre-release-ui.sh \
  /path/to/GoldenCheetah.AppImage artifacts/ui
```

The output directory contains `junit.xml`, application logs, screenshots and,
when `GC_UI_RECORD_VIDEO=1` and `ffmpeg` are available, `session.mp4`.

Required Linux packages are `Xvfb`, `dbus-run-session`, `gdbus`, Python 3,
PyGObject AT-SPI bindings (`python3-pyatspi`) and Python Xlib
(`python3-xlib`). The AppImage is run with extraction mode so FUSE is not a
test prerequisite.
