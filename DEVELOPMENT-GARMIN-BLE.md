# GoldenCheetah Garmin BLE development

This setup separates the reproducible build from hardware validation:

- Build and static development use the Qt 6.8.3 container.
- Live Garmin, BlueZ, D-Bus, and GUI tests run against the host Bluetooth stack.

GoldenCheetah already implements the standard Bluetooth Heart Rate Service. The
proof of concept should therefore validate and diagnose the existing path before
adding a Garmin-specific protocol.

## Build with Docker

From the repository root:

```sh
docker build -f .devcontainer/Dockerfile -t goldencheetah-dev .
docker run --rm -it \
  -v "$PWD:/workspace/GoldenCheetah" \
  -w /workspace/GoldenCheetah \
  goldencheetah-dev \
  bash .devcontainer/build.sh
```

The same Dockerfile is used automatically by VS Code Dev Containers. Build
output is written to `build-devcontainer/`.

## Build a host-runnable AppImage

The project uses the same `linuxdeployqt` and `appimagetool` flow as the
official GoldenCheetah Linux distribution. Package the current container build
from the repository root:

```sh
docker run --rm \
  -v "$PWD:/workspace/GoldenCheetah" \
  -w /workspace/GoldenCheetah \
  goldencheetah-dev \
  bash .devcontainer/package-appimage.sh
```

The resulting executable is
`build-devcontainer/GoldenCheetah-BLE-PoC-x86_64.AppImage`. It contains the Qt
runtime and can be launched directly on the host. If FUSE is unavailable, use
AppImage's extraction mode:

```sh
./build-devcontainer/GoldenCheetah-BLE-PoC-x86_64.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 \
  ./build-devcontainer/GoldenCheetah-BLE-PoC-x86_64.AppImage
```

The uncompressed package is retained under
`build-devcontainer/package-appimage/GoldenCheetah.AppDir/` for diagnostics.

The container intentionally does not store Strava credentials. To test a source
build against Strava, add the client ID and client secret defines to the ignored
`src/gcconfig.pri` file.

## Existing heart-rate path

1. `BT40Controller` discovers BLE devices and applies the configured device
   address filter.
2. `BT40Device` discovers the standard Heart Rate service UUID `0x180d`.
3. It subscribes to Heart Rate Measurement characteristic UUID `0x2a37`.
4. Notifications are decoded as the standard 8-bit or 16-bit BPM value.
5. `BT40Controller::setBPM()` writes the value to `RealtimeData`.
6. The training recorder consumes the `RealtimeData` heart-rate channel.

Relevant implementation files:

- `src/Train/BT40Controller.cpp`
- `src/Train/BT40Controller.h`
- `src/Train/BT40Device.cpp`
- `src/Train/RealtimeData.cpp`
- `src/Train/TrainSidebar.cpp`

## Host-side Garmin proof of concept

1. Enable heart-rate broadcasting on the Garmin watch. Garmin menu names vary
   by model, but the watch must advertise the standard BLE Heart Rate service.
2. Start the watch activity or broadcast mode and keep its display awake for the
   first discovery attempt.
3. On the host, run `bluetoothctl scan on` and verify that the watch appears.
4. Run `btmon` in another terminal while GoldenCheetah scans and connects.
5. In GoldenCheetah, add one Bluetooth 4.0 training device and multi-select the
   trainer and watch on the sensor page.
6. Select that single combined device in Train view and connect it.
7. Start a short recording and verify live power and BPM, then inspect the activity
   for a non-empty heart-rate series.

If discovery succeeds but BPM remains zero, capture the GoldenCheetah debug log
and `btmon` trace. This proof of concept fixes the Qt 6 service-error signal and
logs successful notification subscription plus the first Heart Rate Measurement
payload and decoded BPM value. A Garmin-specific implementation is only needed
if the watch does not expose UUIDs `0x180d` and `0x2a37`.

## One BLE profile for multiple sensors

A Bluetooth 4.0 device profile can contain multiple physical BLE devices. In the
pairing wizard, select the smart trainer and heart-rate broadcaster together. The
resulting single training-device row owns one `BT40Controller`, which merges
power, cadence, speed, trainer control, and heart rate into one realtime stream.

Configured training profiles use 20-second Low Energy scans. If only some
configured sensors are found, discovery restarts after one second and continues
until every sensor is present. This allows Connect to be pressed before a trainer
is powered on or before a watch starts broadcasting.

`Disconnect` stops discovery and pending retries. The debug log and UI report
the first active FTMS or Cycling Power sample and the first decoded heart-rate
sample, which distinguishes a GATT connection from working telemetry.

## Optional container hardware test

Passing the host system D-Bus socket and display into a container can work, but
it depends on host BlueZ and desktop permissions. It is useful for CI-like smoke
tests, not as the primary hardware development loop.

## Standard BLE heart-rate belts

This path is not Garmin-specific. Any sensor that advertises the Bluetooth SIG
Heart Rate service UUID `0x180d` and Heart Rate Measurement characteristic UUID
`0x2a37` can feed BPM into the same realtime stream.

Compatibility requirements:

- The belt must support Bluetooth Low Energy, not only ANT+.
- The belt must be awake; wear it and wet the electrode contacts before scanning.
- Another app or bike computer must not occupy the belt's only BLE connection.
- The belt and trainer should be selected in the same Bluetooth 4.0 profile.

On Linux, a host application without `CAP_NET_ADMIN` may not be able to infer
the remote address type automatically. Qt documents this limitation in
[QLowEnergyController](https://doc.qt.io/qt-6/qlowenergycontroller.html).
The implementation starts with the previously proven random address type and
alternates between random and public after initial connection failures. Once a
type connects successfully, it remains fixed for that device.

## Recovery after a radio drop

When a configured sensor leaves radio range or its broadcast stops, the
controller re-enters Low Energy discovery instead of retrying only the stale
device object. Scans continue until the configured address is advertised again;
the normal reconnect timer then restores GATT services and notifications.

A manual Disconnect still stops discovery, address fallback, and connection
retries immediately.
