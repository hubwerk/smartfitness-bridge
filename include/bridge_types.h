#pragma once
#include <Arduino.h>

struct FitnessTelemetry {
    float speed = 0.0f;
    float cadence = 0.0f;
    float power = 0.0f;
    float heartRate = 0.0f;
    float incline = 0.0f;
    float resistance = 0.0f;
    float targetPower = 0.0f;
    bool trainerDetected = false;
    bool connected = false;
    String deviceName;
    String deviceType;
};

struct ControlCommand {
    float targetPower = 0.0f;
    float targetIncline = 0.0f;
    float targetResistance = 0.0f;
    bool requestTrainerMode = false;
};
