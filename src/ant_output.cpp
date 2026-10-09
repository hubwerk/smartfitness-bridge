#include <Arduino.h>
#include "ant_output.h"

void AntOutput::begin() {
    Serial.println("[AntOutput] begin");
}

void AntOutput::loop() {
    // Future: feed esp32-ant integration
}

void AntOutput::publishTelemetry(const FitnessTelemetry& telemetry) {
    Serial.printf("[AntOutput] telemetry power=%.1f cadence=%.1f speed=%.1f\n",
                  telemetry.power,
                  telemetry.cadence,
                  telemetry.speed);
}

void AntOutput::publishControlTarget(const ControlCommand& command) {
    Serial.printf("[AntOutput] control target power=%.1f incline=%.1f resistance=%.1f\n",
                  command.targetPower,
                  command.targetIncline,
                  command.targetResistance);
}
