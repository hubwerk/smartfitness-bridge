#pragma once
#include <Arduino.h>
#include "bridge_types.h"

class BtDeviceAdapter {
public:
    virtual ~BtDeviceAdapter() = default;

    virtual void begin() = 0;
    virtual void loop() = 0;

    virtual bool scanAndConnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool trainerDetected() const = 0;

    virtual FitnessTelemetry telemetry() const = 0;
    virtual void applyControlCommand(const ControlCommand& cmd) = 0;
};
