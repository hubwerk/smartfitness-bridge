# smartfitness-bridge

ESP32-S3 fitness-machine bridge firmware. The initial firmware foundation is
an Arduino-based PlatformIO project with a hardware-independent bridge core.

## Build

Install PlatformIO Core and run `pio run` to compile the `esp32s3` environment.
The board settings target an ESP32-S3 with 16 MB flash and 8 MB OPI PSRAM
(N16R8). Adjust `platformio.ini` if your board differs.

The current core provides normalized telemetry data, trainer capability
metadata, and capability-gated control routing. Telemetry units are m/s, m,
rpm, W, bpm, and percent incline. Control requests are validated before an
adapter receives them. FTMS input and output are modeled as independently
selectable roles, so either or both can be enabled.

Host-side core tests can be compiled with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
    src/bridge_core.cpp tests/host/test_bridge_core.cpp \
    -o /tmp/test_bridge_core
/tmp/test_bridge_core
```

BLE device adapters, an FTMS GATT implementation, ANT+ FE-C, persistence, and
the Wi-Fi configuration/telemetry UI have not yet been implemented.
