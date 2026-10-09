# ESP32 Smart Fitness Bridge

[![CI](https://github.com/manuelh83/smartfitness-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/manuelh83/smartfitness-bridge/actions/workflows/ci.yml)

PlatformIO firmware foundation for an ESP32-S3 DevKitC-1 N16R8 smart fitness bridge.

## Goal
This project bridges fitness equipment Bluetooth protocols and standardized output protocols such as ANT+ FE-C and Bluetooth FTMS.

## Planned integrations
- Device-side protocol adapters inspired by `cagnulein/qdomyos-zwift`
- ANT+ output layer inspired by `RaemondBW/esp32-ant`

## Architecture
- `smartfitness::BridgeCore` (`include/bridge/bridge_core.hpp`): one canonical core header shared by firmware and host tests. Tracks disconnected, connected/read-only, trainer, and control-ready states.
- `BtDeviceAdapter`: equipment protocol interface. Adapters emit normalized telemetry through a callback and expose connection status and capabilities.
- `AntOutput` / `FtmsOutput`: independent output extension points with telemetry and accepted-target snapshots. Their inbound control callbacks route through the same core gate.
- `WebUi`: currently a bounded, nonblocking Serial console with testable command/status handlers, ready for a future Wi-Fi frontend.
- `ConfigStore`: editable configuration, pairing name, and a saved in-memory snapshot. Save/load works within one instance, **not across reboot**; NVS remains future work.
- `smartfitness::FtmsRoles` (`include/bridge/ftms_roles.hpp`): the single input/output role representation used by `ConfigStore` and the `WebUi` toggles. Either role or both may be enabled independently.

## Working runtime
The firmware boots disconnected. It includes an **explicit simulation adapter**, not a BLE scanner. Nothing is broadcast over ANT+ or FTMS yet.

The loop runs the adapter, refreshes authoritative connection/capability state, fans out changed telemetry and accepted targets, then services enabled outputs. A disconnect or capability change clears the target; disconnect also clears telemetry. Disabled outputs reject incoming commands and discard their snapshots; re-enabling refreshes them on the next loop.

Use the Serial monitor at 115200 baud with newline-terminated commands:

```text
help
status
scan
power 200
incline 5
trainer off
power 250
disconnect
```

`scan` starts a new connection session, clearing any old target, and connects the simulated device with 150 W, 80 rpm, 25 km/h and 120 bpm. Valid controls update its telemetry. `trainer off` switches it to read-only, so the subsequent power command is rejected.

Other commands: `speed <km/h>`, `resistance <%>`, `trainer on`, `ant on/off`, `ftms-input on/off`, `ftms-output on/off`, `save`, `load`, and `unpair`. FTMS input and output settings remain independent; the input setting is reserved for a future FTMS device adapter, not a working BLE role today. Pairing records a name only and does not automatically reconnect.

## Trainer gating
Commands require a connected device, trainer detection, a supported control, a finite value in range, and adapter acceptance. A command cannot promote a device to trainer mode. Rejected commands never replace the accepted target.

Normalized units: speed in km/h, cadence in rpm, power in watts, heart rate in bpm, incline in percent, and resistance in percent. Core safety bounds are speed 0–100 km/h, incline −20–25%, resistance 0–100%, and power 0–2000 W. Real adapters must additionally enforce their equipment's tighter bounds.

## Extension contract
- Implement `BtDeviceAdapter` for a real protocol (inspired by `cagnulein/qdomyos-zwift`), including capability detection, connection lifecycle, and `apply()` returning whether a control was accepted.
- Emit telemetry from the cooperative `loop()` context. Future BLE callbacks on other tasks must queue events for that loop; the core is not thread-safe.
- Subclass `AntOutput` or `FtmsOutput` and override publish/loop methods to implement transport encoding and scheduling. Call the base publish methods to keep snapshots current, and use `requestControl()` for received protocol controls. Override `clearControlTarget()` (calling the base) to clear queued transport targets when disconnected or capabilities are lost; a snapshot target is only meaningful when `hasControlTarget()` is true.
- The attached adapter and outputs must outlive the core. Reattachment removes old callbacks and clears device state.
- ANT+ FE-C integration (inspired by `RaemondBW/esp32-ant`), vendor BLE protocols, discovery/reconnect, stale-telemetry timeouts, NVS, and Wi-Fi/HTTP UI remain for later passes. No upstream code has been copied.

`qdomyos-zwift` is GPL-3.0; any future port of its code must account for that license. This repository does not yet contain a LICENSE file.

## Build target
- ESP32-S3 DevKitC-1 N16R8
- 16 MB Flash
- 8 MB PSRAM
- PlatformIO Arduino framework

```sh
pio run -e esp32s3dev
pio run -e esp32s3dev -t upload
pio device monitor -b 115200
```

Only the Arduino/ESP32 framework is needed for this pass; networking libraries will be added when they are actually used.

## Host tests
The existing standalone assertion-based host test now exercises the same core as firmware, including simulated adapter callbacks, both outputs, control rejection, disconnect clearing, configuration round-trips, and console commands. From the repository root:

```sh
make host-tests
```

The Makefile defines the source list once, compiles with `g++ -std=c++17 -Wall -Wextra -Werror`, and runs `/tmp/smartfitness-core-test`. GNU Make and g++ are required.

## CI
GitHub Actions runs on pushes and pull requests to `main`. The `host-tests` job runs `make host-tests`; the `firmware` job builds `esp32s3dev` with PlatformIO and caches its packages and pip downloads. These checks verify the scaffold builds and its host assertions pass; they do not validate real BLE or ANT+ transport or hardware behavior.
