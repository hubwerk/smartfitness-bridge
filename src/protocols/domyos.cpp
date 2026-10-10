// Domyos / Decathlon bike protocol (Chang Yow init variant).
//
// Ported from qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift,
// src/devices/domyosbike/domyosbike.cpp), Copyright (C) Roberto Viola and
// contributors, licensed under GPL-3.0. This port is part of
// smartfitness-bridge and distributed under the same license.

#include "protocols/domyos.hpp"

#include <cmath>

namespace smartfitness {
namespace protocols {
namespace domyos {

const ble::Uuid kService = ble::Uuid::fromString("49535343-fe7d-4ae5-8fa9-9fafd205e455");
const ble::Uuid kWrite = ble::Uuid::fromString("49535343-8841-43f4-a8d4-ecbe34729bb3");
const ble::Uuid kNotify = ble::Uuid::fromString("49535343-1e4d-4bd9-ba61-23c647249616");

std::vector<std::vector<Bytes>> initSteps() {
    return {
        {{0xf0, 0xc8, 0x01, 0xb9}},
        {{0xf0, 0xc9, 0xb9}},
        {{0xf0, 0xa3, 0x93}},
        {{0xf0, 0xa4, 0x94}},
        {{0xf0, 0xa5, 0x95}},
        {{0xf0, 0xab, 0x9b}},
        {{0xf0, 0xc4, 0x03, 0xb7}},
        // power on bt icon
        {{0xf0, 0xad, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0xff},
         {0xff, 0xff, 0x8b}},
        // power on bt word
        {{0xf0, 0xcb, 0x02, 0x00, 0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00},
         {0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xb6}},
        {{0xf0, 0xad, 0xff, 0xff, 0x00, 0x05, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x01, 0xff},
         {0xff, 0xff, 0x94}},
    };
}

Bytes noOpFrame() { return {0xf0, 0xac, 0x9c}; }

std::vector<Bytes> resistanceFrames(std::uint8_t level) {
    Bytes frame{0xf0, 0xad, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0xff, 0xff, 0xff, 0x00};
    frame[10] = level;
    std::uint8_t sum = 0;
    for (std::size_t index = 0; index + 1 < frame.size(); ++index) sum = static_cast<std::uint8_t>(sum + frame[index]);
    frame[22] = sum;
    return {Bytes(frame.begin(), frame.begin() + 20), Bytes(frame.begin() + 20, frame.end())};
}

namespace {
bool startsWith(const Bytes& frame, std::uint8_t second) {
    return frame.size() >= 2 && frame[0] == 0xf0 && frame[1] == second;
}
}  // namespace

bool FrameAssembler::feed(const std::uint8_t* data, std::size_t length, Bytes& out) {
    if (!data) return false;
    Bytes value(data, data + length);
    if (last_.size() == 20 && ((startsWith(last_, 0xbc) && length == 6) || (startsWith(last_, 0xdb) && length == 7) ||
                               (startsWith(last_, 0xdd) && length == 7))) {
        incomplete_ = false;
        Bytes joined = last_;
        joined.insert(joined.end(), value.begin(), value.end());
        value = joined;
    }
    last_ = value;
    // Any packet other than the expected tail abandons a pending head
    // (upstream keeps the flag latched, which would pause polling forever).
    incomplete_ = false;
    if (value.size() != 26) {
        if (value.size() == 20 && (startsWith(value, 0xbc) || startsWith(value, 0xdb) || startsWith(value, 0xdd)))
            incomplete_ = true;
        return false;
    }
    out = value;
    return true;
}

void FrameAssembler::reset() {
    last_.clear();
    incomplete_ = false;
}

bool decodeStatus(const Bytes& frame, Status& out) {
    if (frame.size() != 26) return false;
    out.speed = static_cast<float>((frame[6] << 8) | frame[7]) / 10.0F;
    out.cadence = static_cast<float>(frame[9]);
    out.distance = static_cast<float>((frame[12] << 8) | frame[13]) / 10.0F;
    out.resistance = frame[14] < 1 ? 1 : frame[14];
    out.heartRate = frame[18];
    return true;
}

float power(int resistance, float cadence) {
    if (cadence <= 0.0F) return 0.0F;
    return static_cast<float>((10.39 + 1.45 * (resistance - 1.0)) * std::exp(0.028 * cadence));
}

}  // namespace domyos

ble::ConnectPlan DomyosSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(domyos::kService, domyos::kNotify);
    return plan;
}

void DomyosSource::reset() {
    SourceProtocol::reset();
    steps_.clear();
    step_ = 0;
    attempts_ = 0;
    stepSentMs_ = 0;
    initializing_ = false;
    initDone_ = false;
    lastPollMs_ = 0;
    assembler_.reset();
}

bool DomyosSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 1U) == 0) {
        setError("Domyos notify characteristic unavailable");
        return false;
    }
    steps_ = domyos::initSteps();
    step_ = 0;
    attempts_ = 0;
    initializing_ = true;
    sendStep(nowMs);
    return true;
}

void DomyosSource::sendStep(std::uint32_t nowMs) {
    ++attempts_;
    stepSentMs_ = nowMs;
    for (const auto& frame : steps_[step_]) queueWrite(domyos::kService, domyos::kWrite, frame);
}

void DomyosSource::tick(std::uint32_t nowMs) {
    if (initializing_) {
        if (nowMs - stepSentMs_ < domyos::kStepTimeoutMs) return;
        if (attempts_ >= domyos::kStepAttempts) {
            initializing_ = false;
            fail("Domyos init step not acknowledged");
            return;
        }
        sendStep(nowMs);
        return;
    }
    if (!initDone_ || assembler_.incomplete() || nowMs - lastPollMs_ < domyos::kPollIntervalMs) return;
    lastPollMs_ = nowMs;
    // Upstream also refreshes the console display once per second; this port
    // only keeps the bike streaming with the no-op poll.
    queueWrite(domyos::kService, domyos::kWrite, domyos::noOpFrame());
}

void DomyosSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (initializing_ && domyos::acknowledgesInit(length)) {
        ++step_;
        attempts_ = 0;
        if (step_ >= steps_.size()) {
            initializing_ = false;
            initDone_ = true;
            lastPollMs_ = nowMs;
        } else {
            sendStep(nowMs);
        }
    }
    domyos::Bytes frame;
    if (!assembler_.feed(data, length, frame)) return;
    domyos::Status status;
    if (!domyos::decodeStatus(frame, status)) return;
    sample_.speed = {status.speed, true};
    sample_.cadence = {status.cadence, true};
    sample_.resistance = {static_cast<float>(status.resistance), true};
    sample_.power = {domyos::power(status.resistance, status.cadence), true};
    if (status.heartRate > 0) sample_.heartRate = {static_cast<float>(status.heartRate), true};
    touch(nowMs);
}

bool DomyosSource::control(const ControlRequest& request, std::uint32_t) {
    if (!initDone_ || request.control != Control::Resistance) return false;
    const int level = percentToLevel(request.value, domyos::kMinResistance, domyos::kMaxResistance);
    bool ok = true;
    for (const auto& frame : domyos::resistanceFrames(static_cast<std::uint8_t>(level)))
        ok = queueWrite(domyos::kService, domyos::kWrite, frame) && ok;
    return ok;
}

TrainerCapabilities DomyosSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    capabilities.setControl(Control::Resistance, initDone_);
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
