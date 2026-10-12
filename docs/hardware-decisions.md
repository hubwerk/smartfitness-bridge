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
- Queues are bounded: 48 central scan reports, 32 events, 8 worker commands,
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

## HD-006: Two firmware variants (web / HMI) on one shared core

**Status:** Accepted

### Context
The bridge should run either headless with a Wi-Fi web UI (ESP32-S3 DevKitC-1
N16R8) or with a local display (ELECROW CrowPanel 1.28" rotary display) offering
the same interactions. Business logic (BLE input, protocols, `BridgeCore`, FTMS
output, configuration, commands) must not be duplicated.

### Decision
- Sources are split into `core` (`include/core`, `src/core`), `variants/web` and
  `variants/hmi`. Each PlatformIO environment compiles `main.cpp`, the core and
  only its own variant directory (`build_src_filter`) and only its own libraries.
- The variant is selected with exactly one of `-DSF_VARIANT_WEB` /
  `-DSF_VARIANT_HMI`; `include/variant.h` fails with `#error` for none or both.
  `main.cpp` runs the shared setup and calls `variant::begin()` / `variant::loop()`.
- The former `WebUi` is split: the variant-neutral `BridgeController` executes
  `Command`s and builds a view model (`bridge_view_model.h`); `web_json` renders it
  for the web variant, the HMI renders it with LVGL.
- The shared configuration schema stays at v3 including the Wi-Fi fields; the HMI
  variant ignores them and never starts Wi-Fi. HMI-only settings (brightness,
  screen timeout, LED ring) live in a separate NVS namespace (`sfhmi`).
- `env:esp32s3dev` keeps its name and behaviour.

### Consequences
- The core is host-tested without any variant source (`make host-tests-core`).
- Without Wi-Fi the HMI variant has more free RAM and no BLE/Wi-Fi coexistence
  load, but no remote API.
- New user-facing features need a `Command` plus a renderer in each variant.

## HD-007: CrowPanel 1.28" HMI hardware, drivers and LVGL version

**Status:** Accepted (not hardware-verified)

### Context
The CrowPanel 1.28" ESP32-S3 Rotary Display (ESP32-S3R8, 8 MB octal PSRAM, 16 MB
flash) has a 240×240 round GC9A01 IPS panel on SPI, a CST816D capacitive touch
controller on I2C, a rotary encoder with push button, PWM backlight and a ring of
WS2812 LEDs. The firmware must stay on Arduino core 2.0.x because NimBLE-Arduino
1.4.3 needs it.

### Decision
- **Pins** only from ELECROW's official GitHub repository (factory firmware,
  lesson examples, schematic *ESP32 Display-1.28-V1.0*, commit `63e407f`), all in
  `include/variants/hmi/board_crowpanel128.h` with per-pin sources. Unclear items
  are marked `TODO(verify)`.
- **Display:** LovyanGFX 1.2.7 (`Panel_GC9A01`, SPI2 at 80 MHz, DMA flush), as in
  ELECROW's examples; it builds on core 2.0.17.
- **Touch:** a small own CST816D reader (single register read, no blocking retries)
  instead of ELECROW's example driver, which retries forever on I2C errors.
- **Encoder:** pin-change interrupts with a quadrature table; LVGL reads the
  accumulated detents through an encoder input device (knob-only operation via
  LVGL groups). Button = press, long press = back.
- **LVGL 9.2.2:** current 9.x release that works with core 2.0.17 and LovyanGFX;
  the same version drives the headless host simulator. Partial rendering with one
  240×40 RGB565 buffer in internal DMA-capable RAM (PSRAM fallback; the flush
  waits for DMA, so a second buffer would not help); built-in
  Montserrat fonts initially (replaced by Antonio in HD-008).
- **LED ring:** Adafruit NeoPixel 1.12.3, optional (`SF_HMI_LED_RING` build flag
  and a runtime setting) for connection/control status.

### Consequences
- Display orientation, touch mapping, encoder direction, LED count/order and
  GPIO 2 (external 5 V enable) must be checked on a real board.
- HD-008 supplies a Latin-1 font with umlauts; the German string table is still
  untranslated and falls back to English.
- Upgrading to LVGL 9.3+ or Arduino core 3.x requires re-checking LovyanGFX and
  NimBLE compatibility.

## HD-008: Shared LCARS-inspired interface design

**Status:** Accepted; simulator/browser verified, not hardware-verified

### Context
The web and round-display variants expose the same bridge commands and safety
rules. Their presentation should share a warm, flat, condensed design language
without affecting the common core, API contract or physical trainer behavior.

### Decision
- Adopt the shared palette, shapes and states documented in
  [the interface design system](hmi/README.md): black background; orange/peach
  swept frames and navigation; blue/violet data; lavender secondary actions;
  red alerts, stop and destructive controls. Focus has a white outline.
- Adapt the frame to the round 240×240 panel with circular segmented gauges,
  safe-area lists, scroll-on-focus pill rows and condensed central readouts.
  The HMI frame is now a header-only top arc with rounded endcaps, numeric
  code and peach separator, never an elbow/sidebar or bottom frame. Widen
  lists by 18 px while retaining round-safe focused targets. A reusable
  left-edge curved scrollbar (4 px dim lavender track, 6 px orange thumb)
  appears only for overflowing lists and adapter/model rollers; visible
  fraction/offset determine its length/position, with an 8° minimum thumb.
  Scroll, focus, roller and layout changes update it; straight LVGL scrollbars
  are disabled. Red-alert state affects only the header arc, not the gauge
  or scrollbar; existing stop/destructive action colours remain.
  Retain the knob/touch paths, speed confirmation and double factory-reset
  confirmation. Web intentionally keeps its elbow, desktop sidebar and
  responsive phone pill row unchanged.
- Use **Antonio Regular**, **SIL OFL 1.1**, with committed source, licence and
  regeneration tooling. HMI text fonts contain Latin-1 at 16/20 px; the 48 px
  font contains digits/punctuation only to avoid wasting flash. Keep the LVGL
  symbol fallback. Embed Latin-1 WOFF2 directly in the gzipped web page.
- Include only original **LCARS-inspired** shapes/colours and the free font:
  no trademarked logos, insignia, ship registry imagery, copied screen assets
  or sounds. No affiliation or endorsement is implied.
- Commit actual LVGL and fixed-fixture Chromium screenshots with before/after
  examples. CI fails on deterministic HMI drift, warns on browser pixel drift,
  and continues to upload both galleries as artifacts.

### Flash and RAM impact

The web font itself is **9,292 bytes** (WOFF2), base64-embedded inside the page.
The deterministic HTML gzip grows from **5,334 to 17,522 bytes (+12,188 B)**.
Removing just the font payload from the final page gives **7,223 bytes**, so
the embedded font's incremental gzip cost is **10,299 bytes**. Final raw HTML
is 34,270 bytes. This remains a small asset relative to the 6.25 MiB app
partition, but the whole firmware budget must be confirmed by the linker.

Baseline firmware measurements from the successful
[main-branch CI run](https://github.com/manuelh83/smartfitness-bridge/actions/runs/38130475907)
at `c153653`:

| Variant | Baseline flash | Baseline static RAM | Post-restyle delta |
|---|---:|---:|---|
| `esp32s3dev` | 1,540,353 B | 73,100 B | Requires successful firmware build |
| `crowpanel128` | 1,456,129 B | 52,284 B | Requires successful firmware build |

Local PlatformIO installation of `espressif32` failed with `HTTPClientError`
before compilation, including on the unchanged baseline. Do not substitute
host ELF sizes or asset bytes for firmware-linked flash/RAM deltas. The CI
firmware matrix remains the authority for those measurements; the configured
app partition is 6,553,600 bytes in both environments.

As an asset-only footprint check, the three generated fonts compile to
**29,480 bytes** total in the host renderer's object files (including host
metadata), while the formerly enabled unused Montserrat sizes are removed.
Segmented gauges create more LVGL objects than a single arc; runtime heap,
PSRAM, flash/RAM deltas, panel readability and colours still need firmware and
hardware measurements.

### Consequences
- No font CDN or additional runtime network resource is required. The font is
  served as part of the existing flash asset; Origin/Content-Type checks and
  the no-CORS policy are unchanged.
- The screenshot browser/font-generation dependencies are development-only;
  there are no new firmware libraries. Generated fonts are committed so normal
  firmware and simulator builds do not require the conversion toolchain.
- Hardware verification remains mandatory, particularly for compact keyboard
  keys, contrast, touch targets and the new gauge's runtime allocation cost.
