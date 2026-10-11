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
    SourceSample record;
    record.power = {static_cast<float>(measurement.power), true};
    if (measurement.crank.present) {
        record.cadence = {crank_.update(measurement.crank, nowMs), crankKnown_};
        crankKnown_ = true;
    }
    if (measurement.wheel.present) {
        record.speed = {sensors::wheelSpeedKmh(wheel_.update(measurement.wheel, nowMs)), wheelKnown_};
        wheelKnown_ = true;
    }
    commitSample(record, nowMs);
}

void CyclingPowerSource::tick(std::uint32_t nowMs) {
    expireSamples(nowMs, 3000);
}

void CyclingPowerSource::reset() {
    SourceProtocol::reset();
    crank_.reset();
    wheel_.reset();
    crankKnown_ = wheelKnown_ = false;
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
    SourceSample record;
    if (measurement.crank.present) {
        record.cadence = {crank_.update(measurement.crank, nowMs), crankKnown_};
        crankKnown_ = true;
    }
    if (measurement.wheel.present) {
        record.speed = {sensors::wheelSpeedKmh(wheel_.update(measurement.wheel, nowMs)), wheelKnown_};
        wheelKnown_ = true;
    }
    commitSample(record, nowMs);
}

void CscSource::tick(std::uint32_t nowMs) {
    expireSamples(nowMs, 3000);
}

void CscSource::reset() {
    SourceProtocol::reset();
    crank_.reset();
    wheel_.reset();
    crankKnown_ = wheelKnown_ = false;
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
    SourceSample record;
    record.heartRate = {static_cast<float>(bpm), true};
    commitSample(record, nowMs);
}

}  // namespace protocols
}  // namespace smartfitness
