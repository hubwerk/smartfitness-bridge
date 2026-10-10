#pragma once
#include <string>

struct FitnessTelemetry {
    float speed = 0.0f;
    float cadence = 0.0f;
    float power = 0.0f;
    float heartRate = 0.0f;
    float incline = 0.0f;
    float resistance = 0.0f;
    float targetPower = 0.0f;
    float distance = 0.0f;
    float elapsed = 0.0f;
    bool distanceValid = false;
    bool elapsedValid = false;
    bool trainerDetected = false;
    bool connected = false;
    std::string deviceName;
    std::string deviceType;
};
