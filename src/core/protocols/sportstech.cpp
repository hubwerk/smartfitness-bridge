// Sportstech / FitHome treadmill BLE protocol (F15, F31, WP100, WP200).
//
// Ported from syssi/esphome-treadmill-f15
// (https://github.com/syssi/esphome-treadmill-f15), Copyright syssi and
// contributors, licensed under the Apache License 2.0.
// Pinned revision 61172d6448e00d0a4eb5d70c3c39a3427c292041:
// components/treadmill_f15/treadmill_f15.{cpp,h}, components/treadmill_f15/
// button/__init__.py, components/treadmill_f15/number/ and docs/protocol.md.
// Also cross-checked against deniskie/ha-sportstech-walkingpad-integration,
// MIT-licensed, revision f2f102710f3a7ca8c0671ea32316cb18b35274fd.
// This port is part of smartfitness-bridge and distributed under
// GPL-3.0-or-later.

#include "protocols/sportstech.hpp"

#include <algorithm>
#include <cmath>

namespace smartfitness {
namespace protocols {
namespace sportstech {

const ble::Uuid kService = ble::Uuid::from16(0xfff0);
const ble::Uuid kNotify = ble::Uuid::from16(0xfff1);
const ble::Uuid kWrite = ble::Uuid::from16(0xfff2);

std::uint8_t checksum(const std::uint8_t* data, std::size_t length) {
    std::uint8_t value = 0;
    for (std::size_t index = 0; index < length; ++index) value ^= data[index];
    return value;
}

Bytes frame(std::uint8_t reg, const Bytes& payload) {
    Bytes out{0x02, reg};
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(checksum(out.data() + 1, out.size() - 1));
    out.push_back(0x03);
    return out;
}

Bytes pollFrame() { return {0x02, 0x51, 0x51, 0x03}; }

Bytes limitsFrame(std::uint8_t subcommand) { return frame(0x50, {subcommand}); }

Bytes stopFrame() { return frame(0x53, {0x03}); }

Bytes speedInclineFrame(std::uint8_t speedTenths, std::uint8_t inclinePercent) {
    return frame(0x53, {0x02, speedTenths, inclinePercent, 0, 0, 0, 0, 0, 0, 0});
}

bool decodeStatus(const std::uint8_t* data, std::size_t length, Status& out) {
    if (!data || length < 5 || length > kMaxFrameLength || data[0] != 0x02 || data[length - 1] != 0x03) return false;
    if (data[1] != 0x51 || checksum(data + 1, length - 3) != data[length - 2]) return false;
    const std::uint8_t type = data[2];
    out = Status{};
    out.mode = static_cast<Mode>(type);
    if (length < 17) return true;  // stopped: 02 51 00 51 03
    auto le16 = [data](std::size_t at) {
        return static_cast<std::uint16_t>(data[at] | (static_cast<unsigned>(data[at + 1]) << 8));
    };
    out.full = true;
    out.speed = static_cast<float>(data[3]) * 0.1F;
    out.incline = static_cast<float>(data[4]);
    out.elapsed = le16(5);
    out.distance = le16(7);
    out.calories = static_cast<float>(le16(9)) * 0.1F;
    out.steps = le16(11);
    out.heartRate = data[13];
    return true;
}

}  // namespace sportstech

using sportstech::Mode;

ble::ConnectPlan SportstechF15Source::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(sportstech::kService, sportstech::kNotify);
    return plan;
}

void SportstechF15Source::reset() {
    SourceProtocol::reset();
    connected_ = false;
    mode_ = Mode::Stopped;
    pollMs_ = 0;
    speedTarget_ = inclineTarget_ = -1;
    minSpeed_ = sportstech::kMinSpeed;
    maxSpeed_ = sportstech::kMaxSpeed;
    minIncline_ = 0.0F;
    maxIncline_ = sportstech::kMaxIncline;
    elapsedOffset_ = 0;
    lastElapsed_ = 0;
    haveElapsed_ = false;
}

bool SportstechF15Source::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 1U) == 0) {
        setError("Sportstech notify characteristic unavailable");
        return false;
    }
    connected_ = true;
    pollMs_ = nowMs;
    return queueWrite(sportstech::kService, sportstech::kWrite, sportstech::limitsFrame(0x02), false) &&
           queueWrite(sportstech::kService, sportstech::kWrite, sportstech::limitsFrame(0x03), false) &&
           queueWrite(sportstech::kService, sportstech::kWrite, sportstech::pollFrame(), false);
}

void SportstechF15Source::tick(std::uint32_t nowMs) {
    if (!connected_ || pendingWrites() || nowMs - pollMs_ < sportstech::kPollIntervalMs) return;
    pollMs_ = nowMs;
    queueWrite(sportstech::kService, sportstech::kWrite, sportstech::pollFrame(), false);
}

void SportstechF15Source::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (data && length >= 7 && length <= sportstech::kMaxFrameLength && data[0] == 0x02 &&
        data[1] == 0x50 && data[length - 1] == 0x03 && sportstech::checksum(data + 1, length - 3) == data[length - 2]) {
        const float minSpeed = std::max(sportstech::kHardMinSpeed, std::min(sportstech::kHardMaxSpeed, data[4] * 0.1F));
        const float maxSpeed = std::max(sportstech::kHardMinSpeed, std::min(sportstech::kHardMaxSpeed, data[3] * 0.1F));
        const float minIncline = std::max(0.0F, std::min(sportstech::kHardMaxIncline, static_cast<float>(data[4])));
        const float maxIncline = std::max(0.0F, std::min(sportstech::kHardMaxIncline, static_cast<float>(data[3])));
        if (data[2] == 0x02 && minSpeed <= maxSpeed) {
            minSpeed_ = minSpeed;
            maxSpeed_ = maxSpeed;
        } else if (data[2] == 0x03 && minIncline <= maxIncline) {
            minIncline_ = minIncline;
            maxIncline_ = maxIncline;
        }
        return;
    }

    sportstech::Status status;
    if (!sportstech::decodeStatus(data, length, status)) return;
    mode_ = status.mode;
    if (mode_ != Mode::Running) speedTarget_ = inclineTarget_ = -1;

    const bool knownMode = mode_ == Mode::Stopped || mode_ == Mode::Finish || mode_ == Mode::Starting ||
                           mode_ == Mode::Running || mode_ == Mode::Stopping || mode_ == Mode::Error ||
                           mode_ == Mode::Safety || mode_ == Mode::Sleep || mode_ == Mode::Paused;
    if (!knownMode) {
        sample_.speed = {0.0F, true};
        sample_.heartRate = {};
        touch(nowMs);
        return;
    }

    if (mode_ == Mode::Stopped || mode_ == Mode::Finish || mode_ == Mode::Starting) {
        elapsedOffset_ = 0;
        lastElapsed_ = 0;
        haveElapsed_ = false;
        if (sample_.elapsed.valid) sample_.elapsed.value = 0.0F;
        if (sample_.distance.valid) sample_.distance.value = 0.0F;
        if (sample_.totalEnergy.valid) sample_.totalEnergy.value = 0.0F;
        if (sample_.stepCount.valid) sample_.stepCount.value = 0.0F;
    }

    if (!status.full) {
        sample_.speed = {0.0F, true};
        sample_.heartRate = {};
        touch(nowMs);
        return;
    }

    if (mode_ != Mode::Running && mode_ != Mode::Paused && mode_ != Mode::Stopping) {
        sample_.speed = {mode_ == Mode::Starting ? status.speed : 0.0F, true};
        sample_.heartRate = {};
        touch(nowMs);
        return;
    }

    sample_.speed = {status.speed, true};
    sample_.incline = {status.incline, true};
    if (mode_ == Mode::Running) {
        if (haveElapsed_ && status.elapsed < lastElapsed_) elapsedOffset_ += lastElapsed_;
        lastElapsed_ = status.elapsed;
        haveElapsed_ = true;
    }
    sample_.elapsed = {static_cast<float>(elapsedOffset_ + status.elapsed), true};
    sample_.distance = {static_cast<float>(status.distance), true};
    sample_.totalEnergy = {status.calories, true};
    sample_.stepCount = {static_cast<float>(status.steps), true};
    if (status.heartRate > 0)
        sample_.heartRate = {static_cast<float>(status.heartRate), true};
    else
        sample_.heartRate = {};
    // A target is kept while the belt ramps towards it so that a request for
    // the other value does not cancel the ramp; once reached, console changes
    // are followed again.
    if (speedTarget_ >= 0 && std::lround(status.speed * 10) == speedTarget_) speedTarget_ = -1;
    if (inclineTarget_ >= 0 && std::lround(status.incline) == inclineTarget_) inclineTarget_ = -1;
    touch(nowMs);
}

bool SportstechF15Source::fresh(std::uint32_t nowMs) const {
    return connected_ && hasData() && nowMs - lastDataMs() <= 2000 && !failed();
}

bool SportstechF15Source::control(const ControlRequest& request, std::uint32_t nowMs) {
    if (!capabilities().supports(request.control) || !std::isfinite(request.value) || !fresh(nowMs)) return false;
    if (request.control == Control::Speed && request.value == 0.0F) {
        if (mode_ != Mode::Running && mode_ != Mode::Starting && mode_ != Mode::Paused) return false;
        speedTarget_ = inclineTarget_ = -1;
        return queueWrite(sportstech::kService, sportstech::kWrite, sportstech::stopFrame(), false);
    }
    // Speed/incline frames are only sent to a running belt; the bridge never
    // starts the treadmill.
    if (mode_ != Mode::Running || !sample_.speed.valid) return false;
    long speed = speedTarget_ >= 0 ? speedTarget_ : std::lround(sample_.speed.value * 10);
    long incline = inclineTarget_ >= 0 ? inclineTarget_ : (sample_.incline.valid ? std::lround(sample_.incline.value) : 0);
    if (request.control == Control::Speed) {
        if (request.value < minSpeed_ || request.value > maxSpeed_) return false;
        speed = std::lround(request.value * 10);
    } else {
        if (request.value < minIncline_ || request.value > maxIncline_) return false;
        incline = std::lround(request.value);
    }
    if (speed < std::lround(minSpeed_ * 10) || speed > std::lround(maxSpeed_ * 10) || incline < std::lround(minIncline_) ||
        incline > std::lround(maxIncline_))
        return false;
    if (!queueWrite(sportstech::kService, sportstech::kWrite,
                    sportstech::speedInclineFrame(static_cast<std::uint8_t>(speed), static_cast<std::uint8_t>(incline)),
                    false))
        return false;
    (request.control == Control::Speed ? speedTarget_ : inclineTarget_) =
        static_cast<int>(request.control == Control::Speed ? speed : incline);
    return true;
}

TrainerCapabilities SportstechF15Source::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    const bool controls = connected_ && hasData() && !failed();
    capabilities.setControl(Control::Speed, controls);
    capabilities.setControl(Control::Incline, controls);
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
