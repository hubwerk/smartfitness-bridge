#pragma once
#include <string>
#include "fitness_metrics.h"

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
    bool speedValid = false;
    bool cadenceValid = false;
    bool powerValid = false;
    bool heartRateValid = false;
    bool inclineValid = false;
    bool resistanceValid = false;
    bool targetPowerValid = false;
    bool distanceValid = false;
    bool elapsedValid = false;
    smartfitness::FitnessMetrics metrics;
    bool trainerDetected = false;
    bool connected = false;
    std::string deviceName;
    std::string deviceType;
};
