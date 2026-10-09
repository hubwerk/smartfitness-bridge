#include <Arduino.h>
#include "ftms_output.h"

void FtmsOutput::begin() {
    Serial.println("[FtmsOutput] begin");
}

void FtmsOutput::loop() {
    // Future: BLE FTMS service and control endpoint
}

void FtmsOutput::publishTelemetry(const FitnessTelemetry& telemetry) {
    Serial.printf("[FtmsOutput] telemetry power=%.1f cadence=%.1f speed=%.1f trainer=%d\n",
                  telemetry.power,
                  telemetry.cadence,
                  telemetry.speed,
                  telemetry.trainerDetected);
}

void FtmsOutput::publishControlTarget(const ControlCommand& command) {
    Serial.printf("[FtmsOutput] control target power=%.1f incline=%.1f resistance=%.1f\n",
                  command.targetPower,
                  command.targetIncline,
                  command.targetResistance);
}
