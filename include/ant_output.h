#pragma once
#include <Arduino.h>
#include "bridge_types.h"

class AntOutput {
public:
    void begin();
    void loop();
    void publishTelemetry(const FitnessTelemetry& telemetry);
    void publishControlTarget(const ControlCommand& command);
};
