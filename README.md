# ESP32 Smart Fitness Bridge

[![CI](https://github.com/manuelh83/smartfitness-bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/manuelh83/smartfitness-bridge/actions/workflows/ci.yml)

PlatformIO firmware foundation for an ESP32-S3 DevKitC-1 N16R8 smart fitness bridge.

> **License:** GPL-3.0-or-later (see [`LICENSE`](LICENSE)). The vendor protocols in
> `src/protocols/` and `include/protocols/` are ported from
> [`cagnulein/qdomyos-zwift`](https://github.com/cagnulein/qdomyos-zwift) (GPL-3.0),
> which makes the firmware a derivative work.

## Goal
This project bridges fitness equipment Bluetooth protocols and standardized output protocols such as Bluetooth FTMS (and, with extra hardware, ANT+ FE-C – see [`docs/hardware-decisions.md`](docs/hardware-decisions.md)).

## Architecture

```text
            Bluetooth LE (one NimBLE stack, central + peripheral)            Wi-Fi (AP / STA)
 ┌───────────────────────────────┐                                    ┌──────────────────────────┐
 │ FTMS / CPS / CSC / HRS /      │  scan reports, notifications       │ Browser SPA (/), /api/*, │
 │ Echelon / Domyos / Yesoul /   │──────────┐                         │ WebSocket /ws            │
 │ Schwinn IC4 / Keiser M3i      │          ▼                         └────────────┬─────────────┘
 └───────────────────────────────┘  NimbleCentral (BLE task, worker)               │ AsyncTCP task
                                    │ bounded queues only                          ▼
                                    ▼                                   WebPortal → ApiExchange
                         BleInputAdapter (loop)                          (bounded job queue,
                         registry + SourceProtocol codecs                 result/snapshot cache)
                         reconnect/backoff, stale timeout                          │
                                    │                                              ▼
 SimulatedDeviceAdapter ──► InputSelector ──► BridgeCore ◄── WebUi (shared command executor) ◄── Serial console
                                              │  routeControl (trainer gate, bounds)
                                              ▼
                                FtmsOutput (GATT server, 0x1826)      AntOutput (placeholder)
```

Everything that runs on the Arduino `loop()` – the core, the adapters' protocol logic, the command executor – is single-threaded. BLE callbacks (NimBLE host task and the central worker task) and HTTP/WebSocket handlers (AsyncTCP task) only push into bounded, lock-protected queues; the loop drains them.

- `smartfitness::BridgeCore` (`include/bridge/bridge_core.hpp`): one canonical core header shared by firmware and host tests. Tracks disconnected, connected/read-only, trainer, and control-ready states.
- `BtDeviceAdapter`: equipment input interface. Implementations: `SimulatedDeviceAdapter`, `BleInputAdapter` (real BLE devices) and `InputSelector`, which forwards to the BLE input when a device is paired and BLE input is enabled, otherwise to the simulation.
- `BleInputAdapter` (`include/ble_input_adapter.h`): scanning, pairing, connection lifecycle, stale telemetry. It talks to the radio through the `CentralTransport` interface (`include/central_transport.h`); `NimbleCentral` implements it on the firmware, `tests/host/fake_central.hpp` in host tests.
- Protocols (`include/protocols/`): pure C++ `SourceProtocol` implementations (FTMS, sensors, vendor ports) and the adapter registry.
- `AntOutput` / `FtmsOutput`: independent outputs. `AntOutput` is still a placeholder; `FtmsOutput` is a working BLE FTMS peripheral (see below).
- FTMS codec (`include/ftms/ftms_codec.hpp`): pure C++ encoding/decoding of FTMS characteristics, used by both the output and the FTMS input.
- `WebUi` (`include/web_ui.h`): the shared command executor. The Serial console and the web API both produce `Command` values (`include/bridge_commands.h`) and run them through `WebUi::execute`; it also renders the status/scan/config JSON views.
- `ApiExchange` (`include/web_api.h`) validates API JSON and hands jobs to the loop; `WebPortal` (`include/web_portal.h`, firmware only) owns Wi-Fi, DNS/captive portal, mDNS and the HTTP/WebSocket server; `WifiPolicy` (`include/wifi_policy.h`) decides AP/STA/fallback.
- `ConfigStore` (`include/config_store.h`): configuration persisted through a `ConfigBackend` (NVS on the firmware, memory in host tests). See [Persistent configuration](#persistent-configuration).
- `smartfitness::FtmsRoles` (`include/bridge/ftms_roles.hpp`): independent input/output role flags. `ftms.input` now means "use the paired BLE device as input (auto-connect)".

## Supported input devices

| Adapter id | Device | Detected by | Telemetry | Control |
|---|---|---|---|---|
| `ftms` | Any FTMS machine: indoor bike (`0x2AD2`), treadmill (`0x2ACD`), cross trainer (`0x2ACE`), rower (`0x2AD1`) | service `0x1826` | per data characteristic; Feature `0x2ACC` and supported ranges define capabilities | Control Point `0x2AD9`: Request Control, target power, resistance, inclination, indoor bike simulation (when the Feature says so) |
| `cps` | Cycling power meter | service `0x1818` | power, cadence (crank revolutions) | – |
| `csc` | Speed/cadence sensor | service `0x1816` | speed (wheel, 2.105 m), cadence | – |
| `hrs` | Heart rate monitor | service `0x180D` | heart rate | – |
| `echelon` | Echelon Connect Sport / EX bikes (non-FTMS) | name `ECH…` (not rower/stride/stair) | cadence, resistance; power and speed derived like upstream | resistance |
| `domyos` | Domyos / Decathlon bikes (non-FTMS) | name `Domyos-Bike…` | speed, cadence, heart rate, resistance; power derived like upstream | resistance |
| `yesoul` | Yesoul bikes | name `YESOUL…`/`YS_G1M_…` or manufacturer data | cadence, power, resistance; speed derived from cadence | – |
| `schwinn-ic4` | Schwinn IC4 / IC8, Bowflex C6/C7/C9 | name `IC Bike…`, `C7-…`, `C9/C10…` | FTMS indoor bike data (read-only, like upstream) | – |
| `keiser-m3i` | Keiser M3i | name `M3…` + valid broadcast | cadence, power, heart rate, gear from the advertisement (no connection) | – |

When several adapters match, the highest priority wins (Keiser 90, Echelon/Domyos 80, Schwinn/Yesoul 70, FTMS 50, CPS 40, CSC 30, HRS 20). The adapter can be overridden when pairing.

The vendor test vectors in `tests/host/test_ble_input.cpp` are built from the upstream parser layouts and the literal command bytes in upstream sources (upstream ships no captured traces); they have not been compared against real equipment.

### Adding a protocol
1. Add a pure C++ `SourceProtocol` in `include/protocols/<name>.hpp` / `src/protocols/<name>.cpp` (no Arduino includes). Declare subscriptions/reads in `plan()`, decode in `onNotify()`/`onRead()`, emit writes from `apply()`/`poll()`. Add the GPL attribution header if the logic is ported.
2. Register it in `kProtocols` in `src/protocols/registry.cpp` with an id (`[a-z0-9_-]`, ≤ 16 chars), a `matches()` function and a priority.
3. Add host tests with byte samples to `tests/host/test_ble_input.cpp` and the new source file to the `Makefile`.

### Connection lifecycle
- The paired device is connected on boot when BLE input is enabled. Connect attempts time out after 15 s; failures and link loss reconnect with exponential backoff (1 s … 30 s).
- If a connected device delivers no data for 5 s, its telemetry is marked stale and invalidated (outputs stop reporting the metrics). If no data arrives for 20 s, the link is dropped and reconnected.
- The peer currently connected to the FTMS output is never offered in scan results and never connected to as an input.

## Serial console
The Serial monitor (115200 baud, newline-terminated) accepts the same commands as the web UI:

| Command | Effect |
|---|---|
| `help`, `status` | Help and a one-line status (includes `input=ble/sim link=… paired=… stale=1`) |
| `scan [seconds]` | BLE scan (1–60 s, default 10 s); without BLE input support it connects the simulation |
| `scan-stop`, `scan-results` | Stop scanning / list results (address, name, RSSI, detected adapter) |
| `pair <aa:bb:cc:dd:ee:ff> [adapter\|auto] [public\|random]` | Pair a scanned device (adapter override optional), persist it and connect |
| `connect`, `disconnect`, `unpair` | Reconnect the paired device, disconnect, forget the pairing |
| `sim` | Use the simulated device as input (150 W, 80 rpm, 25 km/h, 120 bpm) |
| `power <W>`, `resistance <%>`, `incline <%>`, `speed <km/h>` | Control through the `BridgeCore` gate; prints the rejection reason |
| `trainer on/off`, `ant on/off`, `ftms-input on/off`, `ftms-output on/off`, `ftms-name <name>` | Settings |
| `wifi <ssid> [password]`, `wifi-clear`, `ap-pass <password>` | Wi-Fi settings (save + restart to apply) |
| `save`, `load`, `factory-reset`, `restart` | Persistence and reboot |

`status` includes `ftms-out(advertising=…,client=…,control=…)`: whether the FTMS output is advertising, whether a BLE client is connected, and whether that client has been granted control via Request Control.

## Web interface
### Wi-Fi setup
- **No Wi-Fi credentials stored:** the bridge opens a WPA2 access point `SmartFitness-XXXX` (`XXXX` = last two MAC bytes) with the password **`smartfitness`** (change it in Settings or with `ap-pass`). SSID and password are printed on the Serial console at boot. A DNS catch-all and captive-portal probe handlers (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`) open the UI automatically on most phones; otherwise browse to `http://192.168.4.1/`.
- **Credentials stored:** the bridge joins the network as a station and is reachable at `http://smartfitness.local/` (mDNS) or the IP printed on the console. If it is not connected within 20 s (at boot or after losing the network), it additionally opens the access point (AP+STA) while the station keeps retrying; the fallback AP is closed again once the station has been connected for 2 minutes and no client is using the AP.
- Wi-Fi changes are applied after **Save** and a restart.

### Using the UI
The single-page app (`web/index.html`, plain HTML/JS/CSS, gzipped into flash at build time by `scripts/embed_web_assets.py`, no external resources) has four tabs:
- **Dashboard:** live power, cadence, speed, heart rate, incline/resistance target, input/link/stale state, trainer/control state and FTMS output client state (WebSocket, ≤ 4 Hz).
- **Devices:** pairing walkthrough – 1. *Scan* (put the equipment into pairing mode and disconnect it from other apps first), 2. pick a result (name, RSSI, detected type, services), optionally change the adapter type, 3. *Pair*. The pairing is saved immediately and reconnected on boot. *Disconnect*, *Connect* and *Unpair* are on the same page; *Use simulation* switches to the simulated device.
- **Control:** set target power/resistance/incline; the result (`applied`, `not a trainer`, `unsupported control`, …) is shown.
- **Settings:** FTMS output name and on/off, ANT on/off (requires hardware, see `docs/hardware-decisions.md`), Wi-Fi credentials, AP password, save, factory reset, restart and firmware version.

### REST / WebSocket API
All POST bodies are JSON objects (max. 1 KB, unknown keys rejected). The HTTP handlers never touch the core: a valid request is queued for the main loop and answered with **`202 {"id":n}`**; the outcome is pushed over the WebSocket as a `result` message and can be polled with `GET /api/result?id=n` (`202` while pending, `200` when done, `404` unknown/expired). Errors: `400 {"error":"…"}` invalid JSON or values, `413` body too large, `503` queue full.

| Method | Path | Body | Effect |
|---|---|---|---|
| GET | `/api/status` | – | Status snapshot (same as WebSocket `status`) |
| POST | `/api/scan` | `{"seconds":10}` or `{"action":"stop"}` | Start (1–60 s) or stop a scan |
| GET | `/api/scan/results` | – | `{"type":"scan","scanning":…,"devices":[{"address","addressType","name","rssi","services","adapter","connectable"}],"adapters":[…]}` |
| POST | `/api/pair` | `{"address":"aa:bb:cc:dd:ee:ff","adapter":"auto","addressType":"random"}` | Pair a scanned device (adapter/addressType optional), save, connect |
| POST | `/api/unpair` | `{}` | Disconnect and forget the paired device |
| POST | `/api/connect`, `/api/disconnect`, `/api/simulate` | `{}` | Connect paired device / disconnect / use the simulation |
| POST | `/api/control` | `{"control":"power","value":200}` | `power`, `resistance`, `incline` or `speed` through the core gate |
| GET | `/api/config` | – | Settings (passwords are never returned; `wifiPasswordSet` instead) |
| POST | `/api/config` | any of `ftmsName`, `ftmsOutput`, `ftmsInput`, `ant`, `simulatedTrainer`, `wifiSsid` (empty clears), `wifiPassword` (requires `wifiSsid`; omit to keep the stored one), `apPassword`, `save` | Apply settings in order; a rejected setting stops the rest |
| POST | `/api/factory-reset`, `/api/restart` | `{}` | Factory reset / reboot |
| WS | `/ws` | – | Server → client JSON messages with `type` `status` (≤ 4 Hz), `scan`, `config` (on change) and `result`; client messages are ignored |

There is **no authentication** on the API: access is protected only by the WPA2 access point or your home network. Do not expose the bridge to untrusted networks.

## Persistent configuration
Settings changed on the console are edited in RAM; `status` ends with `unsaved=1` while they differ from the stored configuration.

| Command | Effect |
|---|---|
| `save` | Writes the configuration to NVS. Nothing is written when the stored record would not change. |
| `load` | Reloads the configuration from NVS (discarding unsaved edits) and applies it. |
| `unpair` | Disconnects, clears the paired device and persists only that change immediately. |
| `factory-reset` | Disconnects, erases the `sfbridge` NVS namespace and restores the defaults. |

On boot the stored configuration is loaded before the outputs are enabled, so `ftms-name`, the ANT/FTMS toggles, the simulated trainer flag and the pairing survive a reboot. A missing, corrupt or unknown-version record falls back to the defaults (the record is replaced on the next `save`).

NVS layout: namespace `sfbridge`, one blob key `config` (max. 256 bytes), little endian:

| Field | Size | Notes |
|---|---|---|
| magic | 2 | `'S' 'F'` |
| schema version | 1 | currently `1`; other versions are rejected |
| `antEnabled`, `ftms.input`, `ftms.output`, `simulatedTrainer` | 4 × 1 | each must be `0` or `1` |
| `ftmsName` | 1 + n | length ≤ 20, printable ASCII; empty uses `SmartFitness Bridge` |
| `pairedDeviceName` | 1 + n | length ≤ 32, printable ASCII |
| paired source device | 1 + 1 + 6 | reserved for the BLE input adapter: valid flag, address type (0–3), BLE address; all zero when unused |
| CRC-32 | 4 | over all preceding bytes |

To limit flash wear, writes only happen on explicit console commands (`save`, `unpair`, `factory-reset`) from the main loop, never from the BLE task or periodically.

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
The standalone assertion-based host tests exercise the same core as firmware, including simulated adapter callbacks, both outputs, control rejection, disconnect clearing, configuration round-trips, and console commands. `tests/host/test_ftms.cpp` covers the FTMS codec (byte layouts, flags, little-endian encoding, every Control Point opcode including invalid lengths and out-of-range values, result-code mapping) and the `FtmsOutput` control flow without a BLE stack. `tests/host/test_config_store.cpp` covers persistence through `MemoryConfigBackend`: round-trips across `ConfigStore` instances, corrupt and old-version fallback, value validation, `unpair` and `factory-reset`, and that unchanged configurations are not rewritten. From the repository root:

```sh
make host-tests
```

The Makefile defines the source list once, compiles with `g++ -std=c++17 -Wall -Wextra -Werror`, and runs `/tmp/smartfitness-core-test`. GNU Make and g++ are required.

## CI
GitHub Actions runs on pushes and pull requests to `main`. The `host-tests` job runs `make host-tests`; the `firmware` job builds `esp32s3dev` with PlatformIO and caches its packages and pip downloads. These checks verify the scaffold builds and its host assertions pass; they do not validate the real BLE radio, ANT+ transport or hardware behavior.
