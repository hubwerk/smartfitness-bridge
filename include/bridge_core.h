#pragma once
#include <Arduino.h>
#include "bridge_types.h"

class BridgeCore {
public:
    void begin();
    void loop();

    void setTelemetry(const FitnessTelemetry& telemetry);
    const FitnessTelemetry& telemetry() const;

    void setControlCommand(const ControlCommand& cmd);
    bool canControl() const;

private:
    FitnessTelemetry currentTelemetry_;
    ControlCommand currentCommand_;
};
