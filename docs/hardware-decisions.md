# Hardware decisions

## HD-001: ANT+ is not possible with the built-in ESP32-S3 radio

**Status:** Accepted

### Context
The original goal includes ANT+ FE-C output. The ESP32-S3 radio supports Wi-Fi and Bluetooth LE 5 only.
ANT/ANT+ is a separate, proprietary protocol (Garmin/Dynastream) that is only available on licensed
silicon (e.g. Nordic nRF52 with SoftDevice S212/S340, nRF24AP2). Espressif provides no ANT stack and
the radio cannot be reprogrammed to speak ANT.

### Decision
1. **Primary output is Bluetooth LE.** FTMS is implemented for fitness apps such as
   Zwift and MyWhoosh; interoperability is not verified on hardware.
2. **ANT+ FE-C is deferred** and will require external hardware. Preferred option when needed:
   **USB ANT+ stick on the ESP32-S3 USB-OTG port (USB host mode)** speaking the ANT serial protocol.
3. Alternatives, if the USB route fails:
   - nRF52832/nRF52840 with ANT SoftDevice as co-processor via UART/SPI (own firmware required)
   - Ready-made ANT module (nRF24AP2 / D52) via UART (availability may be limited)

### Consequences
- `AntOutput` stays an interface without transport until hardware is chosen.
- The ANT transport must be implemented behind an abstraction (e.g. `AntTransport`) so USB stick and
  UART module can be swapped.
- Wiring/power for USB host mode (5 V on the OTG port) must be documented once implemented.

## HD-002: Garmin watches (e.g. fenix 6 Pro) and Bluetooth FTMS

**Status:** Accepted

### Context
Garmin fenix 6 / 6 Pro can pair Bluetooth LE sensors (heart rate, speed/cadence, cycling power),
but smart-trainer control ("Indoor trainer" / FE-C) on this watch generation is **ANT+ only**.
The fenix 6 does not act as a Bluetooth FTMS client. (Verify against the current Garmin firmware
release notes before relying on this.)

### Decision
- To record bridge data on a fenix 6 without ANT+, add standard BLE services in addition to FTMS:
  - **Cycling Power Service** `0x1818` (power, cadence via crank revolutions)
  - **Heart Rate Service** `0x180D` (if the source provides heart rate)
  - optionally **Cycling Speed and Cadence** `0x1816`
- Trainer **control** from the watch (ERG/resistance) requires the ANT+ FE-C path from HD-001.

### Consequences
- BLE CPS/HRS outputs become a cheap, hardware-free intermediate step before ANT+.
- The current "one client at a time" limitation of `FtmsOutput` must be lifted
  (multi-connection advertising and per-client state)
  if a watch and an app (e.g. Zwift) should be connected simultaneously.

## HD-003: BLE central + peripheral + Wi-Fi on one ESP32-S3

**Status:** Accepted; not verified on hardware

### Context
Real equipment requires a BLE central while the FTMS output is a peripheral.
The web portal also needs Wi-Fi on the same ESP32-S3 radio. NimBLE-Arduino 1.4.3
uses the Arduino-ESP32 core 2.0.17 API baseline; NimBLE 2.x needs core 3.x.
`platformio.ini` pins NimBLE 1.4.3 but leaves the `espressif32` platform unpinned,
so it does not independently lock the core to 2.0.17.

### Decision
- Share one NimBLE stack between `NimbleCentral` and `FtmsOutput`, with
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3` (one source, one FTMS client, one spare).
- Keep **Wi-Fi modem sleep enabled** (`WiFi.setSleep(true)` in `WebPortal::begin`);
  this is required by the current coexistence design, not an optional optimization.
- Run blocking central GAP/GATT operations on the `ble-central` worker task.
  BLE callbacks enqueue scan reports/events and FTMS Control Point writes;
  protocol decoding, control gating and state mutation stay in the main loop.
- Hand HTTP requests to the same loop through `ApiExchange`; do not access
  `BridgeCore` from BLE or AsyncTCP callbacks.

### Consequences
- The connection budget is not multi-client FTMS support: advertising currently
  stops with one client, and only one input is selected (no sensor aggregation).
- Queues are bounded: 24 central scan reports, 32 events, 8 worker commands,
  8 FTMS writes, 8 API jobs. Overflow can drop work.
- The central worker has a 6144-byte stack, pinned to core 1. Wi-Fi/FTMS load,
  scanning, reconnect latency and runtime heap/PSRAM headroom require hardware tests.
- Upgrading the platform/core or disabling modem sleep requires renewed
  compatibility/coexistence validation.

## HD-004: ESPAsyncWebServer rather than core WebServer

**Status:** Accepted

### Context
The UI needs concurrent HTTP requests, WebSocket telemetry and BLE work without
moving mutable bridge state into networking callbacks. The Arduino core
`WebServer` is synchronous; the current implementation uses ESPAsyncWebServer
and its AsyncTCP backend.

### Decision
- Use pinned **ESPAsyncWebServer 3.12.1** and **AsyncTCP 3.5.0** with ArduinoJson
  **7.4.3** for REST validation and snapshots.
- Async handlers validate JSON, enqueue commands and read locked result/snapshot
  caches. The main loop executes up to two API jobs per iteration.
- Serve the self-contained gzipped web app from flash; provide captive DNS/probe
  redirects, mDNS, REST and server-to-client `/ws` telemetry.

### Consequences
- Extra dependencies and an AsyncTCP task are required; callbacks must not call
  `BridgeCore` or NVS directly.
- API bodies are limited to 1024 bytes, job capacity to 8 and retained results to
  16. `202` means queued, not applied; clients must observe results.
- Status broadcasts are limited to 4 Hz; scan changes are checked every 1000 ms
  (also refreshed after API jobs).
  Configuration snapshots are available through GET, not broadcast on `/ws`.
- There is currently no API authentication or TLS. Restrict the network and
  change the default AP password; authentication remains roadmap work.

## HD-005: GPL-3.0 licensing for qdomyos-zwift ports

**Status:** Accepted

### Context
Echelon, Domyos, Yesoul and Keiser protocol implementations derive from
[`cagnulein/qdomyos-zwift`](https://github.com/cagnulein/qdomyos-zwift),
Copyright Roberto Viola and contributors, licensed under GPL-3.0. Discovery
rules and the read-only Schwinn FTMS variant also follow upstream behavior.
These are derived implementations, not merely a future inspiration.

### Decision
- Distribute the firmware under **GPL-3.0-or-later**, as stated in the README;
  retain the existing GPL v3 text in `LICENSE`.
- Preserve upstream attribution headers and source paths in the port files.
  Document the derivation and per-device attribution in the README and
  `supported-devices.md`.
- Future ports must retain licensing/attribution and record their upstream revision.

### Consequences
- Firmware redistribution must satisfy GPL obligations, including corresponding
  source; vendor-port additions cannot be treated as proprietary code.
- Dependency licenses remain separate and must also be respected.
- Existing headers record upstream source paths but not a commit revision.
  Host vectors based on those layouts are not evidence of hardware compatibility.
