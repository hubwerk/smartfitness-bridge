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
- `AntOutput` / `FtmsOutput`: independent outputs with trainer-state, telemetry and accepted-target snapshots. Their inbound control callbacks route through the same core gate. `AntOutput` is still a placeholder; `FtmsOutput` is a working BLE FTMS peripheral (see below).
- FTMS codec (`include/ftms/ftms_codec.hpp`, `src/ftms_codec.cpp`): pure C++ encoding/decoding of FTMS characteristics and Control Point opcodes, with no Arduino/NimBLE dependency so it is covered by host tests.
- `WebUi`: currently a bounded, nonblocking Serial console with testable command/status handlers, ready for a future Wi-Fi frontend.
- `ConfigStore`: editable configuration, pairing name, and a saved in-memory snapshot. Save/load works within one instance, **not across reboot**; NVS remains future work.
- `smartfitness::FtmsRoles` (`include/bridge/ftms_roles.hpp`): the single input/output role representation used by `ConfigStore` and the `WebUi` toggles. Either role or both may be enabled independently.

## Working runtime
The firmware boots disconnected. It includes an **explicit simulation adapter**, not a BLE scanner. Telemetry is broadcast over **Bluetooth LE FTMS** (when `ftms-output` is on); nothing is broadcast over ANT+ yet.

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

Other commands: `speed <km/h>`, `resistance <%>`, `trainer on`, `ant on/off`, `ftms-input on/off`, `ftms-output on/off`, `ftms-name <name>`, `save`, `load`, and `unpair`. FTMS input and output settings remain independent; the input setting is reserved for a future FTMS device adapter, not a working BLE role today. Pairing records a name only and does not automatically reconnect.

`status` ends with `ftms-out(advertising=…,client=…,control=…)`: whether the FTMS output is advertising, whether a BLE client is connected, and whether that client has been granted control via Request Control.

## FTMS output (Bluetooth LE)
`FtmsOutput` is a NimBLE-Arduino GATT server (firmware only; the transport is compiled under `#ifdef ARDUINO`, so host tests need no BLE stack). It advertises the Fitness Machine Service UUID `0x1826`, FTMS service data (machine available, indoor bike), and the configured name (`ftms-name`, default `SmartFitness Bridge`, max 20 ASCII characters).

| Characteristic | UUID | Properties | Content |
|---|---|---|---|
| Fitness Machine Feature | `0x2ACC` | read | Derived from the device capabilities: cadence, inclination, heart rate, power; targets: inclination (+ indoor bike simulation), resistance, power |
| Indoor Bike Data | `0x2AD2` | notify | Speed (0.01 km/h), cadence (0.5 rpm), power (W, sint16), heart rate (bpm). A field and its flag are present only when the core marks the metric valid. Sent on change (at most every 250 ms) and at least once per second while a client is connected |
| Training Status | `0x2AD3` | read, notify | Idle without a device, Manual Mode when a device is connected |
| Supported Inclination Range | `0x2AD5` | read | −20.0 … 25.0 %, step 0.1 % |
| Supported Resistance Level Range | `0x2AD6` | read | 0.0 … 100.0, step 1.0 (one unit = one normalized percent) |
| Supported Power Range | `0x2AD8` | read | 0 … 2000 W, step 1 W |
| Fitness Machine Control Point | `0x2AD9` | write, indicate | Opcodes below; every write is answered with `0x80, opcode, result` |
| Fitness Machine Status | `0x2ADA` | notify | Sent for acknowledged changes (reset, start, stop/pause, target incline/resistance/power, simulation parameters) |

Supported Control Point opcodes: Request Control `0x00`, Reset `0x01`, Set Target Inclination `0x03` (sint16, 0.1 %), Set Target Resistance `0x04` (uint8 0.1, sint16 0.1 also accepted), Set Target Power `0x05` (sint16 W), Start/Resume `0x07`, Stop/Pause `0x08` (`0x01` stop, `0x02` pause), and Set Indoor Bike Simulation Parameters `0x11` (grade is mapped to the incline control). Other opcodes return Not Supported (`0x02`); wrong lengths return Invalid Parameter (`0x03`).

Control flow: NimBLE callbacks (BLE task) only push writes and connect/disconnect events into a small lock-protected queue. `FtmsOutput::loop()` drains it, requires Request Control before any other command (otherwise Control Not Permitted `0x05`), and routes set commands through `requestControl()` → `BridgeCore::routeControl`. Core results map to FTMS result codes: Applied → Success `0x01`, Unsupported → Not Supported `0x02`, InvalidValue → Invalid Parameter `0x03`, AdapterRejected → Operation Failed `0x04`, Disconnected/NotTrainer/NoAdapter → Control Not Permitted `0x05`. A BLE disconnect revokes control, drops queued writes from that session and restarts advertising. `ftms-output off` disconnects the client and stops advertising; `ftms-output on` resumes advertising.

### Testing with nRF Connect or Zwift
1. Flash the firmware, open the Serial monitor and run `scan` to connect the simulation (so the Feature characteristic reports capabilities before a client connects).
2. In nRF Connect, scan for `SmartFitness Bridge` (service `0x1826`), connect, and enable notifications on Indoor Bike Data: you should see `44 02 C4 09 A0 00 96 00 78` (25 km/h, 80 rpm, 150 W, 120 bpm) about once per second.
3. Enable indications on the Control Point, write `00` (Request Control → `80 00 01`), then `05 C8 00` (Set Target Power 200 W → `80 05 01`). `status` on the Serial console now shows `power=200.0W`, and Indoor Bike Data reports 200 W.
4. Run `trainer off` and write `05 FA 00` again: the response is `80 05 05` (Control Not Permitted). `trainer on` restores control.
5. In Zwift or MyWhoosh, pair the bridge as power source, cadence and controllable trainer; ERG mode and simulation grade are routed through the same gate.

### Limitations
- Telemetry still comes from `SimulatedDeviceAdapter`; there is no BLE input adapter yet.
- Start/Resume and Stop/Pause are acknowledged (with status notifications) but do not change the simulation; Reset only releases control and does not clear the core target.
- Only one client is served at a time (advertising stops while connected). No bonding/security, no Cycling Power or Heart Rate services, and no total distance, elapsed time or energy fields.
- The Feature characteristic follows the connected device's capabilities; clients usually read it once on connection, so connect the source device before pairing the app.
- Changing the name while a client is connected applies on the next advertising cycle.

## Trainer gating
Commands require a connected device, trainer detection, a supported control, a finite value in range, and adapter acceptance. A command cannot promote a device to trainer mode. Rejected commands never replace the accepted target.

Normalized units: speed in km/h, cadence in rpm, power in watts, heart rate in bpm, incline in percent, and resistance in percent. Core safety bounds are speed 0–100 km/h, incline −20–25%, resistance 0–100%, and power 0–2000 W. Real adapters must additionally enforce their equipment's tighter bounds.

## Extension contract
- Implement `BtDeviceAdapter` for a real protocol (inspired by `cagnulein/qdomyos-zwift`), including capability detection, connection lifecycle, and `apply()` returning whether a control was accepted.
- Emit telemetry from the cooperative `loop()` context. BLE callbacks on other tasks must queue events for that loop (as `FtmsOutput` does); the core is not thread-safe.
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

Dependencies: the Arduino/ESP32 framework (`espressif32` platform, Arduino core 2.0.x) and `h2zero/NimBLE-Arduino@1.4.3` for the FTMS output. Networking libraries will be added when they are actually used.

## Host tests
The standalone assertion-based host tests exercise the same core as firmware, including simulated adapter callbacks, both outputs, control rejection, disconnect clearing, configuration round-trips, and console commands. `tests/host/test_ftms.cpp` covers the FTMS codec (byte layouts, flags, little-endian encoding, every Control Point opcode including invalid lengths and out-of-range values, result-code mapping) and the `FtmsOutput` control flow without a BLE stack. From the repository root:

```sh
make host-tests
```

The Makefile defines the source list once, compiles with `g++ -std=c++17 -Wall -Wextra -Werror`, and runs `/tmp/smartfitness-core-test`. GNU Make and g++ are required.

## CI
GitHub Actions runs on pushes and pull requests to `main`. The `host-tests` job runs `make host-tests`; the `firmware` job builds `esp32s3dev` with PlatformIO and caches its packages and pip downloads. These checks verify the scaffold builds and its host assertions pass; they do not validate the real BLE radio, ANT+ transport or hardware behavior.
