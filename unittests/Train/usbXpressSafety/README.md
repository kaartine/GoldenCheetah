# USBXpress safety regression harness

This Qt Test target compiles the production `src/Train/USBXpress.cpp` and ANT
transport code on Linux with `WIN32` and `GC_HAVE_USBXPRESS`. The test-owned
`windows.h` and `SiUSBXp.h` expose only the API surface used by the adapter, so
neither the proprietary Silicon Labs SDK nor USB hardware is required.
`QtPlatformShim.h` lets Qt detect the real Linux target before the test restores
the production adapter's `WIN32` feature gate.

The suite covers failed device-count and VID/PID queries, canonical
`0fcf:1004` selection, every setup call after `SI_Open`, rollback through
`SI_Close`, actual transfer counts, and close-result propagation. Its ANT
integration case forces libusb discovery to fail, opens the fake Garmin USB1
stick, and verifies that `ANT::closePort()` closes it through USBXpress rather
than the generic Windows handle API. Failed timeout configuration is rejected
before an ANT worker can start blocking I/O.

The non-Windows test matrix includes this target through
`unittests/unittests.pro`.

Build out of tree:

```sh
mkdir -p /tmp/gc-usbxpress-safety
cd /tmp/gc-usbxpress-safety
qmake /path/to/GoldenCheetah-src/unittests/Train/usbXpressSafety/usbXpressSafety.pro
make -j"$(nproc)"
./usbXpressSafety
```
