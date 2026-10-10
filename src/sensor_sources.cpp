#include "protocols/sensor_sources.hpp"

namespace smartfitness {
namespace protocols {
namespace {

bool requireSubscription(std::uint32_t mask) { return (mask & 1U) != 0; }

}  // namespace

TrainerCapabilities SensorSourceBase::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    return capabilities;
}

ble::ConnectPlan CyclingPowerSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(ble::Uuid::from16(sensors::kCyclingPowerService),
                   ble::Uuid::from16(sensors::kCyclingPowerMeasurement));
    return plan;
}

bool CyclingPowerSource::onConnected(std::uint32_t subscribedMask, std::uint32_t) {
    if (requireSubscription(subscribedMask)) return true;
    setError("cycling power measurement unavailable");
    return false;
}

void CyclingPowerSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length,
                                  std::uint32_t nowMs) {
    sensors::CyclingPowerMeasurement measurement;
    if (index != 0 || !sensors::decodeCyclingPower(data, length, measurement)) return;
    sample_.power = {static_cast<float>(measurement.power), true};
    if (measurement.crank.present) sample_.cadence = {crank_.update(measurement.crank, nowMs), true};
    if (measurement.wheel.present)
        sample_.speed = {sensors::wheelSpeedKmh(wheel_.update(measurement.wheel, nowMs)), true};
    touch(nowMs);
}

void CyclingPowerSource::tick(std::uint32_t nowMs) {
    if (sample_.cadence.valid) sample_.cadence.value = crank_.current(nowMs);
    if (sample_.speed.valid) sample_.speed.value = sensors::wheelSpeedKmh(wheel_.current(nowMs));
}

void CyclingPowerSource::reset() {
    SourceProtocol::reset();
    crank_.reset();
    wheel_.reset();
}

ble::ConnectPlan CscSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(ble::Uuid::from16(sensors::kCscService), ble::Uuid::from16(sensors::kCscMeasurement));
    return plan;
}

bool CscSource::onConnected(std::uint32_t subscribedMask, std::uint32_t) {
    if (requireSubscription(subscribedMask)) return true;
    setError("CSC measurement unavailable");
    return false;
}

void CscSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    sensors::CscMeasurement measurement;
    if (index != 0 || !sensors::decodeCsc(data, length, measurement)) return;
    if (measurement.crank.present) sample_.cadence = {crank_.update(measurement.crank, nowMs), true};
    if (measurement.wheel.present)
        sample_.speed = {sensors::wheelSpeedKmh(wheel_.update(measurement.wheel, nowMs)), true};
    touch(nowMs);
}

void CscSource::tick(std::uint32_t nowMs) {
    if (sample_.cadence.valid) sample_.cadence.value = crank_.current(nowMs);
    if (sample_.speed.valid) sample_.speed.value = sensors::wheelSpeedKmh(wheel_.current(nowMs));
}

void CscSource::reset() {
    SourceProtocol::reset();
    crank_.reset();
    wheel_.reset();
}

ble::ConnectPlan HeartRateSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(ble::Uuid::from16(sensors::kHeartRateService), ble::Uuid::from16(sensors::kHeartRateMeasurement));
    return plan;
}

bool HeartRateSource::onConnected(std::uint32_t subscribedMask, std::uint32_t) {
    if (requireSubscription(subscribedMask)) return true;
    setError("heart rate measurement unavailable");
    return false;
}

void HeartRateSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length,
                               std::uint32_t nowMs) {
    std::uint16_t bpm = 0;
    if (index != 0 || !sensors::decodeHeartRate(data, length, bpm)) return;
    sample_.heartRate = {static_cast<float>(bpm), true};
    touch(nowMs);
}

}  // namespace protocols
}  // namespace smartfitness
