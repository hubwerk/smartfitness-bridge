#pragma once

// Normalized telemetry metrics shared by every input protocol, the core and
// all outputs. The set covers every data field of the Bluetooth Fitness
// Machine Service (FTMS) machine profiles; the FTMS codec maps each profile's
// wire types onto these units (see docs/supported-devices.md).
//
// Values are append-only: the enum indexes capability bitmasks and tables.

#include <cstddef>
#include <cstdint>

namespace smartfitness {

enum class Metric : std::uint8_t {
    Speed,                  // km/h
    Distance,               // m (total)
    Cadence,                // rpm; steps/min (cross trainer, climbers); strokes/min (rower)
    Power,                  // W
    HeartRate,              // bpm
    Incline,                // %
    ElapsedTime,            // s
    Resistance,             // device resistance level as reported (unitless)
    AverageSpeed,           // km/h
    AverageCadence,         // same unit as Cadence
    AveragePower,           // W
    TotalEnergy,            // kcal
    EnergyPerHour,          // kcal/h
    EnergyPerMinute,        // kcal/min
    MetabolicEquivalent,    // MET
    RemainingTime,          // s
    RampAngle,              // degrees
    PositiveElevationGain,  // m
    NegativeElevationGain,  // m
    Pace,                   // s per 500 m; 0 = not moving
    AveragePace,            // s per 500 m
    StrokeCount,            // strokes (rower)
    StepCount,              // steps (step climber)
    StrideCount,            // strides (cross trainer, stair climber)
    Floors,                 // floors (step/stair climber)
    ForceOnBelt,            // N (treadmill)
    Count
};

constexpr std::size_t kMetricCount = static_cast<std::size_t>(Metric::Count);
static_assert(kMetricCount <= 32, "metric capabilities are a 32-bit mask");

// Stable lower camel case key (web JSON, diagnostics).
const char* metricKey(Metric metric);

// One optional metric value: valid is false when the source did not send it.
struct MetricSample {
    float value = 0.0F;
    bool valid = false;
};

// One sample of every metric. Named members keep protocol code readable;
// operator[] gives generic access by Metric.
struct MetricSet {
    MetricSample speed;
    MetricSample distance;
    MetricSample cadence;
    MetricSample power;
    MetricSample heartRate;
    MetricSample incline;
    MetricSample elapsed;
    MetricSample resistance;
    MetricSample averageSpeed;
    MetricSample averageCadence;
    MetricSample averagePower;
    MetricSample totalEnergy;
    MetricSample energyPerHour;
    MetricSample energyPerMinute;
    MetricSample metabolicEquivalent;
    MetricSample remainingTime;
    MetricSample rampAngle;
    MetricSample positiveElevationGain;
    MetricSample negativeElevationGain;
    MetricSample pace;
    MetricSample averagePace;
    MetricSample strokeCount;
    MetricSample stepCount;
    MetricSample strideCount;
    MetricSample floors;
    MetricSample forceOnBelt;

    MetricSample& operator[](Metric metric);
    const MetricSample& operator[](Metric metric) const;
};

}  // namespace smartfitness
