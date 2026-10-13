#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "bridge/metric.hpp"

namespace smartfitness {
enum class TreadmillOperation : std::uint8_t {
    Stopped = 0,
    Starting = 2,
    Running = 3,
    Stopping = 4
};
}

// Snapshot emitted by an input adapter. Every metric is optional: a value the
// device did not send stays empty (never 0) and is not processed downstream.
struct FitnessTelemetry {
    std::optional<float> speed;
    std::optional<float> distance;
    std::optional<float> cadence;
    std::optional<float> power;
    std::optional<float> heartRate;
    std::optional<float> incline;
    std::optional<float> elapsed;
    std::optional<float> resistance;
    std::optional<float> averageSpeed;
    std::optional<float> averageCadence;
    std::optional<float> averagePower;
    std::optional<float> totalEnergy;
    std::optional<float> energyPerHour;
    std::optional<float> energyPerMinute;
    std::optional<float> metabolicEquivalent;
    std::optional<float> remainingTime;
    std::optional<float> rampAngle;
    std::optional<float> positiveElevationGain;
    std::optional<float> negativeElevationGain;
    std::optional<float> pace;
    std::optional<float> averagePace;
    std::optional<float> strokeCount;
    std::optional<float> stepCount;
    std::optional<float> strideCount;
    std::optional<float> floors;
    std::optional<float> forceOnBelt;
    // Last control target acknowledged by the adapter (not a measurement).
    std::optional<float> targetPower;
    std::optional<smartfitness::TreadmillOperation> treadmillOperation;
    bool trainerDetected = false;
    bool connected = false;
    std::string deviceName;
    std::string deviceType;

    std::optional<float>& metric(smartfitness::Metric metric);
    const std::optional<float>& metric(smartfitness::Metric metric) const;
};
