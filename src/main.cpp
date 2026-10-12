// Firmware entry point shared by both build variants: sets up the core
// (BLE input, BridgeCore, FTMS output, configuration, Serial console) and
// hands over to the selected variant through variant::begin()/variant::loop().
#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP32)
#include <nvs_flash.h>

// Allocate 24 KB stack for Arduino loopTask (LVGL 9 requires ~10 KB peak during rendering).
#if defined(SET_LOOP_TASK_STACK_SIZE)
SET_LOOP_TASK_STACK_SIZE(24 * 1024);
#else
extern "C" size_t getArduinoLoopTaskStackSize(void) {
    return 24 * 1024;
}
#endif
#endif

#include "variant.h"
#include "bridge/bridge_core.hpp"
#include "ant_output.h"
#include "ftms_output.h"
#include "bridge_controller.h"
#include "config_store.h"
#include "bt_device_adapter.h"
#include "ble_input_adapter.h"
#include "nimble_central.h"

SimulatedDeviceAdapter simulationAdapter;
NimbleCentral bleCentral;
BleInputAdapter bleInput(bleCentral, [] { return static_cast<std::uint32_t>(millis()); });
InputSelector inputSelector(simulationAdapter, &bleInput);
AntOutput antOutput;
FtmsOutput ftmsOutput;
NvsConfigBackend configBackend;
ConfigStore configStore(configBackend);
smartfitness::BridgeCore bridge;
BridgeController controller(bridge, configStore, simulationAdapter, antOutput, ftmsOutput);

namespace {
std::uint32_t restartAtMs = 0;

// The FTMS output's own client is never offered as or connected to as an input.
void updateExcludedPeer() {
    std::array<std::uint8_t, 6> address{};
    std::uint8_t type = 0;
    smartfitness::ble::BleAddress peer;
    const bool valid = ftmsOutput.clientAddress(address, type);
    peer.bytes = address;
    peer.type = type;
    bleInput.setExcludedPeer(peer, valid);
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.printf("SmartFitness Bridge %s (%s variant) starting...\n", SMARTFITNESS_VERSION, variant::name());

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP32)
    esp_err_t nvsErr = nvs_flash_init();
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[NVS] truncating/re-initializing corrupted partition...");
        nvs_flash_erase();
        nvsErr = nvs_flash_init();
    }
    if (nvsErr != ESP_OK) {
        Serial.printf("[NVS] init failed: 0x%x\n", nvsErr);
    }
#endif

    // Load the persisted configuration before outputs are enabled (controller.begin applies it).
    switch (configStore.begin()) {
        case ConfigLoadResult::Loaded: Serial.println("[Config] loaded from NVS"); break;
        case ConfigLoadResult::Missing: Serial.println("[Config] no saved configuration; using defaults"); break;
        case ConfigLoadResult::Invalid: Serial.println("[Config] saved configuration invalid; using defaults"); break;
    }

    if (!bleCentral.begin()) Serial.println("[BLE] central could not be started");
    bridge.attach(inputSelector, antOutput, ftmsOutput);
    bridge.begin();
    controller.attachBleInput(inputSelector, bleInput);
    controller.begin();  // applies the configuration and auto-connects the paired device

    const BridgeConfig& config = configStore.config();
    variant::begin(controller, config);
    if (!config.pairedSource.valid) Serial.println("[BridgeCore] no paired device; use scan + pair, or sim");
}

void loop() {
    updateExcludedPeer();
    bridge.loop();
    controller.loop();
    variant::loop(controller);
    if (controller.restartRequested()) {
        // Give the HTTP/WebSocket response (or the HMI message) time to leave before rebooting.
        if (restartAtMs == 0) restartAtMs = millis() + 1000;
        if (static_cast<std::int32_t>(millis() - restartAtMs) >= 0) ESP.restart();
    }
    delay(5);
}
