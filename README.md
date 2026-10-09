# ESP32 Smart Fitness Bridge

PlatformIO firmware scaffold for an ESP32-S3 DevKitC-1 N16R8 smart fitness bridge.

## Goal
This project bridges fitness equipment Bluetooth protocols and standardized output protocols such as ANT+ FE-C and Bluetooth FTMS.

## Planned integrations
- Device-side protocol adapters inspired by `cagnulein/qdomyos-zwift`
- ANT+ output layer inspired by `RaemondBW/esp32-ant`

## Architecture
- `BridgeCore`: central routing and synchronization logic
- `BtDeviceAdapter`: device-side Bluetooth abstraction
- `AntOutput`: ANT+ FE-C output layer
- `FtmsOutput`: Bluetooth FTMS output layer
- `WebUi`: Wi-Fi configuration and telemetry frontend
- `ConfigStore`: persistent settings and pairing state

## Trainer gating
Control commands are only forwarded when the connected sport device is recognized as a trainer.
Without trainer capability, the system remains read-only on the control side.

## Build target
- ESP32-S3 DevKitC-1 N16R8
- 16 MB Flash
- 8 MB PSRAM
- PlatformIO Arduino framework
