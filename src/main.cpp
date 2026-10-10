#include <Arduino.h>
#include "bridge/bridge_core.hpp"
#include "ant_output.h"
#include "ftms_output.h"
#include "web_ui.h"
#include "web_api.h"
#include "web_portal.h"
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
WebUi webUi(bridge, configStore, simulationAdapter, antOutput, ftmsOutput);
ApiExchange webApi;
WebPortal webPortal(webUi, webApi);

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
    Serial.printf("SmartFitness Bridge %s starting...\n", SMARTFITNESS_VERSION);

    // Load the persisted configuration before outputs are enabled (webUi.begin applies it).
    switch (configStore.begin()) {
        case ConfigLoadResult::Loaded: Serial.println("[Config] loaded from NVS"); break;
        case ConfigLoadResult::Missing: Serial.println("[Config] no saved configuration; using defaults"); break;
        case ConfigLoadResult::Invalid: Serial.println("[Config] saved configuration invalid; using defaults"); break;
    }

    if (!bleCentral.begin()) Serial.println("[BLE] central could not be started");
    bridge.attach(inputSelector, antOutput, ftmsOutput);
    bridge.begin();
    webUi.attachBleInput(inputSelector, bleInput);
    webUi.begin();  // applies the configuration and auto-connects the paired device

    const BridgeConfig& config = configStore.config();
    webPortal.begin(config.wifiSsid, config.wifiPassword, config.apPassword);
    Serial.println("[Web] http://smartfitness.local/ (or the address shown above)");
    if (!config.pairedSource.valid) Serial.println("[BridgeCore] no paired device; use scan + pair, or sim");
}

void loop() {
    updateExcludedPeer();
    bridge.loop();
    webUi.loop();
    webPortal.loop();
    if (webUi.restartRequested()) {
        // Give the HTTP/WebSocket response time to leave before rebooting.
        if (restartAtMs == 0) restartAtMs = millis() + 1000;
        if (static_cast<std::int32_t>(millis() - restartAtMs) >= 0) ESP.restart();
    }
    delay(5);
}
