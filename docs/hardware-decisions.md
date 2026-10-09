# Hardware decisions

## HD-001: ANT+ is not possible with the built-in ESP32-S3 radio

**Status:** Accepted

### Context
The original goal includes ANT+ FE-C output. The ESP32-S3 radio supports Wi-Fi and Bluetooth LE 5 only.
ANT/ANT+ is a separate, proprietary protocol (Garmin/Dynastream) that is only available on licensed
silicon (e.g. Nordic nRF52 with SoftDevice S212/S340, nRF24AP2). Espressif provides no ANT stack and
the radio cannot be reprogrammed to speak ANT.

### Decision
1. **Primary output is Bluetooth LE.** FTMS (already implemented) covers Zwift, MyWhoosh, Rouvy, TrainerRoad.
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
- Garmin devices often allow only one BLE central connection per peripheral; the current
  "one client at a time" limitation of `FtmsOutput` must be lifted (multi-connection advertising)
  if a watch and an app (e.g. Zwift) should be connected simultaneously.
