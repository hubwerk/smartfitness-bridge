#pragma once

#include <array>
#include <cmath>

namespace smartfitness {

struct MetricSample {
    float value = 0;
    bool valid = false;
};

// Units follow FTMS: km/h, rpm, W, bpm, %, device levels, m and seconds.
// Pace is profile-specific (treadmill: km/min; rower: seconds/500 m).
struct FitnessMetrics {
    MetricSample speed;
    MetricSample cadence;
    MetricSample power;
    MetricSample heartRate;
    MetricSample incline;
    MetricSample resistance;
    MetricSample distance;
    MetricSample elapsed;
    MetricSample averageSpeed;
    MetricSample averageCadence;
    MetricSample averagePower;
    MetricSample rampAngle;  // degrees; never inferred from inclination
    MetricSample positiveElevationGain;
    MetricSample negativeElevationGain;
    MetricSample pace;
    MetricSample averagePace;
    MetricSample totalEnergy;  // kcal
    MetricSample energyPerHour;
    MetricSample energyPerMinute;
    MetricSample metabolicEquivalent;
    MetricSample remaining;
    MetricSample forceOnBelt;  // N
    MetricSample stepRate;  // steps/min
    MetricSample averageStepRate;
    MetricSample strideCount;
    MetricSample strokeRate;  // strokes/min
    MetricSample averageStrokeRate;
    MetricSample strokeCount;
    MetricSample floors;
    MetricSample stepCount;
    MetricSample movementDirection;  // 0 forward, 1 backward

    void merge(const FitnessMetrics& fragment);
};

using FitnessMetricField = MetricSample FitnessMetrics::*;
inline constexpr std::array<FitnessMetricField, 31> kFitnessMetricFields{{
    &FitnessMetrics::speed, &FitnessMetrics::cadence, &FitnessMetrics::power,
    &FitnessMetrics::heartRate, &FitnessMetrics::incline, &FitnessMetrics::resistance,
    &FitnessMetrics::distance, &FitnessMetrics::elapsed, &FitnessMetrics::averageSpeed,
    &FitnessMetrics::averageCadence, &FitnessMetrics::averagePower, &FitnessMetrics::rampAngle,
    &FitnessMetrics::positiveElevationGain, &FitnessMetrics::negativeElevationGain,
    &FitnessMetrics::pace, &FitnessMetrics::averagePace, &FitnessMetrics::totalEnergy,
    &FitnessMetrics::energyPerHour, &FitnessMetrics::energyPerMinute,
    &FitnessMetrics::metabolicEquivalent, &FitnessMetrics::remaining,
    &FitnessMetrics::forceOnBelt, &FitnessMetrics::stepRate, &FitnessMetrics::averageStepRate,
    &FitnessMetrics::strideCount, &FitnessMetrics::strokeRate, &FitnessMetrics::averageStrokeRate,
    &FitnessMetrics::strokeCount, &FitnessMetrics::floors, &FitnessMetrics::stepCount,
    &FitnessMetrics::movementDirection
}};

inline void FitnessMetrics::merge(const FitnessMetrics& fragment) {
    for (const auto field : kFitnessMetricFields)
        if ((fragment.*field).valid && std::isfinite((fragment.*field).value))
            this->*field = fragment.*field;
}

}  // namespace smartfitness
