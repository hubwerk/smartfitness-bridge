#include <Arduino.h>
#include "bridge_core.h"

void BridgeCore::begin() {
    Serial.println("[BridgeCore] begin");
}

void BridgeCore::loop() {
    // Future: routing, sync, timeout handling, output fan-out
}

void BridgeCore::setTelemetry(const FitnessTelemetry& telemetry) {
    currentTelemetry_ = telemetry;
    Serial.printf("[BridgeCore] telemetry: device=%s type=%s trainer=%d connected=%d\n",
                  telemetry.deviceName.c_str(),
                  telemetry.deviceType.c_str(),
                  telemetry.trainerDetected,
                  telemetry.connected);
}

const FitnessTelemetry& BridgeCore::telemetry() const {
    return currentTelemetry_;
}

void BridgeCore::setControlCommand(const ControlCommand& cmd) {
    currentCommand_ = cmd;
    if (canControl()) {
        Serial.printf("[BridgeCore] control: power=%.1f incline=%.1f resistance=%.1f\n",
                      cmd.targetPower,
                      cmd.targetIncline,
                      cmd.targetResistance);
    } else {
        Serial.println("[BridgeCore] control rejected: trainer not detected");
    }
}

bool BridgeCore::canControl() const {
    return currentTelemetry_.trainerDetected && currentTelemetry_.connected;
}
