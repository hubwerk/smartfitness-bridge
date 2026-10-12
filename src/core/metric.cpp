#include "bridge/metric.hpp"
#include "bridge_types.h"

namespace smartfitness {
namespace {

struct MetricInfo {
    const char* key;
    MetricSample MetricSet::*sample;
    std::optional<float> FitnessTelemetry::*telemetry;
};

// Indexed by Metric.
constexpr MetricInfo kMetrics[] = {
    {"speed", &MetricSet::speed, &FitnessTelemetry::speed},
    {"distance", &MetricSet::distance, &FitnessTelemetry::distance},
    {"cadence", &MetricSet::cadence, &FitnessTelemetry::cadence},
    {"power", &MetricSet::power, &FitnessTelemetry::power},
    {"heartRate", &MetricSet::heartRate, &FitnessTelemetry::heartRate},
    {"incline", &MetricSet::incline, &FitnessTelemetry::incline},
    {"elapsed", &MetricSet::elapsed, &FitnessTelemetry::elapsed},
    {"resistance", &MetricSet::resistance, &FitnessTelemetry::resistance},
    {"averageSpeed", &MetricSet::averageSpeed, &FitnessTelemetry::averageSpeed},
    {"averageCadence", &MetricSet::averageCadence, &FitnessTelemetry::averageCadence},
    {"averagePower", &MetricSet::averagePower, &FitnessTelemetry::averagePower},
    {"totalEnergy", &MetricSet::totalEnergy, &FitnessTelemetry::totalEnergy},
    {"energyPerHour", &MetricSet::energyPerHour, &FitnessTelemetry::energyPerHour},
    {"energyPerMinute", &MetricSet::energyPerMinute, &FitnessTelemetry::energyPerMinute},
    {"metabolicEquivalent", &MetricSet::metabolicEquivalent, &FitnessTelemetry::metabolicEquivalent},
    {"remainingTime", &MetricSet::remainingTime, &FitnessTelemetry::remainingTime},
    {"rampAngle", &MetricSet::rampAngle, &FitnessTelemetry::rampAngle},
    {"positiveElevationGain", &MetricSet::positiveElevationGain, &FitnessTelemetry::positiveElevationGain},
    {"negativeElevationGain", &MetricSet::negativeElevationGain, &FitnessTelemetry::negativeElevationGain},
    {"pace", &MetricSet::pace, &FitnessTelemetry::pace},
    {"averagePace", &MetricSet::averagePace, &FitnessTelemetry::averagePace},
    {"strokeCount", &MetricSet::strokeCount, &FitnessTelemetry::strokeCount},
    {"stepCount", &MetricSet::stepCount, &FitnessTelemetry::stepCount},
    {"strideCount", &MetricSet::strideCount, &FitnessTelemetry::strideCount},
    {"floors", &MetricSet::floors, &FitnessTelemetry::floors},
    {"forceOnBelt", &MetricSet::forceOnBelt, &FitnessTelemetry::forceOnBelt},
};
static_assert(sizeof(kMetrics) / sizeof(kMetrics[0]) == kMetricCount, "one entry per metric");

const MetricInfo& info(Metric metric) {
    const auto index = static_cast<std::size_t>(metric);
    return kMetrics[index < kMetricCount ? index : 0];
}

}  // namespace

const char* metricKey(Metric metric) {
    return static_cast<std::size_t>(metric) < kMetricCount ? info(metric).key : "unknown";
}

MetricSample& MetricSet::operator[](Metric metric) { return this->*info(metric).sample; }
const MetricSample& MetricSet::operator[](Metric metric) const { return this->*info(metric).sample; }

}  // namespace smartfitness

std::optional<float>& FitnessTelemetry::metric(smartfitness::Metric metric) {
    return this->*smartfitness::info(metric).telemetry;
}

const std::optional<float>& FitnessTelemetry::metric(smartfitness::Metric metric) const {
    return this->*smartfitness::info(metric).telemetry;
}
