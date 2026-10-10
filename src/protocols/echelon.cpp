// Echelon Connect Sport bike protocol.
//
// Ported from qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift,
// src/devices/echelonconnectsport/echelonconnectsport.cpp), Copyright (C)
// Roberto Viola and contributors, licensed under GPL-3.0. This port is part
// of smartfitness-bridge and distributed under the same license.

#include "protocols/echelon.hpp"

namespace smartfitness {
namespace protocols {
namespace echelon {

const ble::Uuid kService = ble::Uuid::fromString("0bf669f1-45f2-11e7-9598-0800200c9a66");
const ble::Uuid kWrite = ble::Uuid::fromString("0bf669f2-45f2-11e7-9598-0800200c9a66");
const ble::Uuid kNotify1 = ble::Uuid::fromString("0bf669f3-45f2-11e7-9598-0800200c9a66");
const ble::Uuid kNotify2 = ble::Uuid::fromString("0bf669f4-45f2-11e7-9598-0800200c9a66");

namespace {

Bytes withChecksum(Bytes frame) {
    std::uint8_t sum = 0;
    for (auto byte : frame) sum = static_cast<std::uint8_t>(sum + byte);
    frame.push_back(sum);
    return frame;
}

// Default table from upstream (33 resistance levels x cadence 0..100 rpm in
// 10 rpm steps). The first column is upstream's Epsilon, i.e. 0 W.
constexpr float kWattTable[33][11] = {
    {0.0F, 1.0F, 2.2F, 4.8F, 9.5F, 13.6F, 16.7F, 22.6F, 26.3F, 29.2F, 47.0F},
    {0.0F, 1.0F, 2.2F, 4.8F, 9.5F, 13.6F, 16.7F, 22.6F, 26.3F, 29.2F, 47.0F},
    {0.0F, 1.3F, 3.0F, 5.4F, 10.4F, 14.5F, 18.5F, 24.6F, 27.6F, 33.5F, 49.5F},
    {0.0F, 1.5F, 3.7F, 6.7F, 11.7F, 15.9F, 19.6F, 26.1F, 30.8F, 35.2F, 51.2F},
    {0.0F, 1.6F, 4.7F, 7.5F, 13.7F, 17.6F, 22.6F, 29.0F, 36.9F, 42.6F, 57.2F},
    {0.0F, 1.8F, 5.2F, 8.0F, 14.8F, 19.1F, 23.5F, 32.5F, 37.5F, 50.8F, 61.8F},
    {0.0F, 1.9F, 5.7F, 8.7F, 15.6F, 20.2F, 25.5F, 33.5F, 39.6F, 52.1F, 65.3F},
    {0.0F, 2.0F, 6.2F, 9.5F, 16.8F, 21.8F, 28.1F, 37.0F, 42.8F, 57.8F, 68.4F},
    {0.0F, 2.1F, 6.8F, 10.8F, 18.2F, 23.6F, 29.5F, 40.0F, 47.6F, 60.5F, 72.1F},
    {0.0F, 2.2F, 7.3F, 11.5F, 19.3F, 26.3F, 33.5F, 45.3F, 51.8F, 66.7F, 76.8F},
    {0.0F, 2.4F, 7.9F, 12.7F, 20.8F, 29.8F, 37.6F, 52.2F, 56.2F, 73.5F, 83.6F},
    {0.0F, 2.6F, 8.5F, 13.5F, 23.5F, 33.6F, 41.9F, 55.1F, 59.0F, 78.6F, 89.7F},
    {0.0F, 2.7F, 9.1F, 14.2F, 25.6F, 35.4F, 45.3F, 57.3F, 62.8F, 81.3F, 95.0F},
    {0.0F, 2.9F, 9.6F, 16.8F, 29.1F, 37.5F, 49.6F, 62.5F, 69.0F, 84.7F, 99.3F},
    {0.0F, 3.0F, 10.0F, 22.3F, 31.2F, 40.3F, 51.8F, 65.0F, 70.0F, 92.6F, 108.2F},
    {0.0F, 3.2F, 10.4F, 24.0F, 36.6F, 42.5F, 56.3F, 74.0F, 85.0F, 98.2F, 123.5F},
    {0.0F, 3.5F, 10.9F, 25.1F, 38.5F, 47.6F, 65.4F, 83.0F, 93.0F, 114.8F, 136.8F},
    {0.0F, 3.7F, 11.5F, 26.0F, 41.0F, 53.2F, 71.6F, 90.0F, 100.0F, 121.7F, 149.2F},
    {0.0F, 4.0F, 12.1F, 27.5F, 43.6F, 56.0F, 82.3F, 101.0F, 113.6F, 143.0F, 162.8F},
    {0.0F, 4.2F, 12.7F, 29.7F, 46.7F, 64.2F, 87.9F, 109.2F, 128.9F, 154.0F, 172.3F},
    {0.0F, 4.5F, 13.7F, 32.0F, 50.0F, 71.8F, 95.6F, 113.8F, 135.6F, 165.0F, 185.0F},
    {0.0F, 4.7F, 14.9F, 34.5F, 54.2F, 77.0F, 100.7F, 127.0F, 147.6F, 180.0F, 200.0F},
    {0.0F, 5.0F, 15.8F, 36.5F, 58.3F, 83.4F, 110.1F, 136.0F, 168.1F, 196.0F, 213.5F},
    {0.0F, 5.6F, 17.0F, 39.5F, 64.3F, 88.8F, 123.4F, 154.0F, 182.0F, 210.0F, 235.0F},
    {0.0F, 6.1F, 18.2F, 44.0F, 70.7F, 99.9F, 133.3F, 166.0F, 198.0F, 230.0F, 253.5F},
    {0.0F, 6.8F, 19.4F, 49.0F, 79.0F, 108.8F, 147.2F, 185.0F, 217.0F, 255.2F, 278.0F},
    {0.0F, 7.6F, 22.0F, 54.8F, 88.0F, 127.0F, 167.0F, 212.0F, 244.0F, 287.0F, 305.0F},
    {0.0F, 8.7F, 26.0F, 62.0F, 100.0F, 145.0F, 190.0F, 242.0F, 281.0F, 315.1F, 350.0F},
    {0.0F, 9.2F, 30.0F, 71.0F, 114.4F, 161.6F, 215.1F, 275.1F, 317.0F, 358.5F, 390.0F},
    {0.0F, 9.8F, 36.0F, 82.5F, 134.5F, 195.3F, 252.5F, 313.7F, 360.0F, 420.3F, 460.0F},
    {0.0F, 10.5F, 43.0F, 95.0F, 157.1F, 228.4F, 300.1F, 374.1F, 403.8F, 487.8F, 540.0F},
    {0.0F, 12.5F, 48.0F, 99.3F, 162.2F, 232.9F, 310.4F, 400.3F, 435.5F, 530.5F, 589.0F},
    {0.0F, 13.0F, 53.0F, 102.0F, 170.3F, 242.0F, 320.0F, 427.9F, 475.2F, 570.0F, 625.0F}};

}  // namespace

std::vector<Bytes> initFrames() {
    const Bytes init1{0xf0, 0xa1, 0x00, 0x91};
    const Bytes init2{0xf0, 0xa3, 0x00, 0x93};
    const Bytes init3{0xf0, 0xb0, 0x01, 0x01, 0xa2};
    return {init1, init1, init1, init1, init2, init1, init3};
}

Bytes pollFrame(std::uint8_t counter) { return withChecksum({0xf0, 0xa0, 0x01, counter}); }
Bytes resistanceFrame(std::uint8_t level) { return withChecksum({0xf0, 0xb1, 0x01, level}); }

bool decode(const std::uint8_t* data, std::size_t length, Frame& out) {
    out = {};
    if (!data) return false;
    if (length == 5 && data[0] == 0xf0 && data[1] == 0xd2) {
        out.kind = Frame::Kind::Resistance;
        out.resistance = data[3];
        return true;
    }
    if (length != 13) return false;
    out.kind = Frame::Kind::Data;
    out.cadence = static_cast<float>(data[10]);
    return true;
}

float power(int resistance, float cadence) {
    if (cadence <= 0.0F) return 0.0F;
    int level = resistance < 0 ? 0 : resistance;
    if (level > 32) level = 32;
    const float* row = kWattTable[level];
    const int step = static_cast<int>(cadence / 10.0F);
    if (step >= 10) return cadence / 100.0F * row[10];
    const float base = row[step];
    return (row[step + 1] - base) / 10.0F * static_cast<float>(static_cast<int>(cadence) % 10) + base;
}

}  // namespace echelon

ble::ConnectPlan EchelonSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(echelon::kService, echelon::kNotify1);
    plan.subscribe(echelon::kService, echelon::kNotify2);
    return plan;
}

void EchelonSource::reset() {
    SourceProtocol::reset();
    ready_ = false;
    counter_ = 1;
    lastPollMs_ = 0;
    resistance_ = 0;
}

bool EchelonSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 0x3U) == 0) {
        setError("Echelon notify characteristics unavailable");
        return false;
    }
    for (const auto& frame : echelon::initFrames()) queueWrite(echelon::kService, echelon::kWrite, frame);
    ready_ = true;
    lastPollMs_ = nowMs;
    return true;
}

void EchelonSource::tick(std::uint32_t nowMs) {
    if (!ready_ || nowMs - lastPollMs_ < echelon::kPollIntervalMs) return;
    lastPollMs_ = nowMs;
    queueWrite(echelon::kService, echelon::kWrite, echelon::pollFrame(counter_));
    counter_ = static_cast<std::uint8_t>(counter_ + 1);
    if (counter_ == 0) counter_ = 1;
}

void EchelonSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    echelon::Frame frame;
    if (!echelon::decode(data, length, frame)) return;
    if (frame.kind == echelon::Frame::Kind::Resistance) {
        resistance_ = frame.resistance;
        sample_.resistance = {static_cast<float>(resistance_), true};
        return;
    }
    sample_.cadence = {frame.cadence, true};
    sample_.speed = {echelon::speedFromCadence(frame.cadence), true};
    sample_.power = {echelon::power(resistance_, frame.cadence), true};
    touch(nowMs);
}

bool EchelonSource::control(const ControlRequest& request, std::uint32_t) {
    if (!ready_ || request.control != Control::Resistance) return false;
    const int level = percentToLevel(request.value, echelon::kMinResistance, echelon::kMaxResistance);
    return queueWrite(echelon::kService, echelon::kWrite, echelon::resistanceFrame(static_cast<std::uint8_t>(level)));
}

TrainerCapabilities EchelonSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    capabilities.setControl(Control::Resistance, ready_);
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
