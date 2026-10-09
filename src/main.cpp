#include <Arduino.h>
#include "bridge/bridge_core.hpp"
#include "ant_output.h"
#include "ftms_output.h"
#include "web_ui.h"
#include "config_store.h"
#include "bt_device_adapter.h"

SimulatedDeviceAdapter deviceAdapter;
AntOutput antOutput;
FtmsOutput ftmsOutput;
ConfigStore configStore;
smartfitness::BridgeCore bridge;
WebUi webUi(bridge, configStore, deviceAdapter, antOutput, ftmsOutput);

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("SmartFitness Bridge starting...");

    configStore.begin();

    bridge.attach(deviceAdapter, antOutput, ftmsOutput);
    bridge.begin();
    webUi.begin();
    Serial.println("[BridgeCore] disconnected; use scan to connect the simulation");
}

void loop() {
    bridge.loop();
    webUi.loop();
    delay(10);
}
