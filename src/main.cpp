// Firmware entry point shared by both build variants: sets up the core
// (BLE input, BridgeCore, FTMS/ANT+ FE-C output, configuration, Serial console) and
// hands over to the selected variant through variant::begin()/variant::loop().
#include <Arduino.h>
#include "debug_serial.h"
#include "variant.h"
#include "bridge/bridge_core.hpp"
#include "ant_output.h"
#include "ftms_output.h"
#include "bridge_controller.h"
#include "config_store.h"
#include "bt_device_adapter.h"
#include "ble_input_adapter.h"
#include "nimble_central.h"

#if defined(SF_VARIANT_HMI)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

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
#ifdef DEBUG_WEB
    debugweb::begin();
#endif
    DBG_SERIAL.printf("SmartFitness Bridge %s (%s variant) starting...\n", SMARTFITNESS_VERSION, variant::name());

    // Load the persisted configuration before outputs are enabled (controller.begin applies it).
    switch (configStore.begin()) {
        case ConfigLoadResult::Loaded: DBG_SERIAL.println("[Config] loaded from NVS"); break;
        case ConfigLoadResult::Missing: DBG_SERIAL.println("[Config] no saved configuration; using defaults"); break;
        case ConfigLoadResult::Invalid: DBG_SERIAL.println("[Config] saved configuration invalid; using defaults"); break;
    }

    if (!bleCentral.begin()) DBG_SERIAL.println("[BLE] central could not be started");
    antOutput.setTransport(variant::antTransport());
    antOutput.setClock([] { return static_cast<std::uint32_t>(millis()); });
    // Stable, per-device ANT+ device number from the factory MAC.
    const std::uint64_t mac = ESP.getEfuseMac();
    antOutput.setDeviceNumber(static_cast<std::uint16_t>((mac >> 32) ^ (mac >> 16)));
    bridge.attach(inputSelector, antOutput, ftmsOutput);
    bridge.begin();
    controller.attachBleInput(inputSelector, bleInput);
    controller.begin();  // applies the configuration and auto-connects the paired device

    const BridgeConfig& config = configStore.config();
    variant::begin(controller, config);
    if (!config.pairedSource.valid) DBG_SERIAL.println("[BridgeCore] no paired device; use scan + pair, or sim");
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
