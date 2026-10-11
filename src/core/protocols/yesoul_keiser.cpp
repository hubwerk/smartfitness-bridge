// Yesoul bike and Keiser M3i protocols.
//
// Ported from qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift,
// src/devices/yesoulbike/yesoulbike.cpp and src/devices/m3ibike/m3ibike.cpp),
// Copyright (C) Roberto Viola and contributors, licensed under GPL-3.0. This
// port is part of smartfitness-bridge and distributed under the same license.

#include "protocols/yesoul_keiser.hpp"

namespace smartfitness {
namespace protocols {
namespace yesoul {

ble::Bytes initFrame() { return {0xf5, 0x20, 0x20, 0x40, 0xf6}; }

bool decode(const std::uint8_t* data, std::size_t length, Frame& out) {
    out = {};
    if (!data || length != 12) return false;
    out.resistance = data[4];
    out.cadence = static_cast<float>(data[6]);
    out.power = static_cast<float>((data[7] << 8) + data[8]);
    out.distance = static_cast<float>((data[2] << 8) | data[3]) / 100.0F;
    return true;
}

}  // namespace yesoul

namespace keiser {

bool parse(const std::uint8_t* data, std::size_t length, Broadcast& out) {
    if (!data || length < 4 || length > 19) return false;
    std::size_t index = 0;
    if (data[0] == 2 && data[1] == 1) index += 2;
    const std::uint8_t major = data[index++];
    const std::uint8_t minor = data[index++];
    if (major != 0x06 || length <= index + 13) return false;
    Broadcast parsed;
    parsed.major = major;
    parsed.minor = minor;
    parsed.systemId = data[index + 1];
    parsed.cadence = static_cast<float>(data[index + 2] | (data[index + 3] << 8)) / 10.0F;
    parsed.heartRate = static_cast<float>(data[index + 4] | (data[index + 5] << 8)) / 10.0F;
    parsed.power = static_cast<float>(data[index + 6] | (data[index + 7] << 8));
    parsed.calories = static_cast<std::uint16_t>(data[index + 8] | (data[index + 9] << 8));
    parsed.seconds = static_cast<std::uint16_t>(data[index + 10] * 60 + data[index + 11]);
    const std::uint16_t distance = static_cast<std::uint16_t>(data[index + 12] | (data[index + 13] << 8));
    parsed.distanceKm = (distance & 0x8000U) ? static_cast<float>(distance & 0x7FFFU) / 10.0F
                                             : static_cast<float>(distance) / 10.0F * 1.60934F;
    parsed.gear = (minor >= 0x21 && length > index + 14) ? data[index + 14] : 0;
    out = parsed;
    return true;
}

}  // namespace keiser

ble::ConnectPlan YesoulSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(ble::Uuid::from16(yesoul::kService), ble::Uuid::from16(yesoul::kNotify));
    return plan;
}

bool YesoulSource::onConnected(std::uint32_t subscribedMask, std::uint32_t) {
    if ((subscribedMask & 1U) == 0) {
        setError("Yesoul notify characteristic unavailable");
        return false;
    }
    queueWrite(ble::Uuid::from16(yesoul::kService), ble::Uuid::from16(yesoul::kWrite), yesoul::initFrame());
    return true;
}

void YesoulSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    yesoul::Frame frame;
    if (!yesoul::decode(data, length, frame)) return;
    SourceSample record;
    record.cadence = {frame.cadence, true};
    record.power = {frame.power, true};
    record.speed = {yesoul::speedFromCadence(frame.cadence), true};
    record.resistance = {static_cast<float>(frame.resistance), true};
    record.distance = {frame.distance * 1000.0F, true};
    commitSample(record, nowMs);
}

TrainerCapabilities YesoulSource::capabilities() const {
    // Upstream never writes resistance to Yesoul bikes, so this is read-only.
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    return capabilities;
}

void KeiserM3iSource::onAdvertisement(const ble::Advertisement& advertisement, std::uint32_t nowMs) {
    keiser::Broadcast broadcast;
    if (!keiser::parse(advertisement.manufacturerData.data(), advertisement.manufacturerData.size(), broadcast)) return;
    SourceSample record;
    record.cadence = {broadcast.cadence, true};
    record.power = {broadcast.power, true};
    record.heartRate = {broadcast.heartRate, true};
    std::size_t offset = advertisement.manufacturerData[0] == 2 && advertisement.manufacturerData[1] == 1 ? 2 : 0;
    if (broadcast.minor >= 0x21 && advertisement.manufacturerData.size() > offset + 16)
        record.resistance = {static_cast<float>(broadcast.gear), true};
    record.distance = {broadcast.distanceKm * 1000.0F, true};
    record.elapsed = {static_cast<float>(broadcast.seconds), true};
    record.totalEnergy = {static_cast<float>(broadcast.calories), true};
    commitSample(record, nowMs);
}

TrainerCapabilities KeiserM3iSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
