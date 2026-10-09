#include <Arduino.h>
#include "bridge_core.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "web_ui.h"
#include "config_store.h"

BridgeCore bridge;
AntOutput antOutput;
FtmsOutput ftmsOutput;
WebUi webUi;
ConfigStore configStore;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("SmartFitness Bridge starting...");

    configStore.begin();
    configStore.load();

    bridge.begin();
    antOutput.begin();
    ftmsOutput.begin();
    webUi.begin();

    FitnessTelemetry telemetry;
    telemetry.deviceName = "placeholder";
    telemetry.deviceType = "ftms";
    telemetry.connected = true;
    telemetry.trainerDetected = false;
    bridge.setTelemetry(telemetry);
}

void loop() {
    bridge.loop();
    antOutput.loop();
    ftmsOutput.loop();
    webUi.loop();
    delay(10);
}
