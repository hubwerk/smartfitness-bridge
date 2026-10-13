// Sportstech F15 / F31 treadmill BLE protocol.
//
// Ported from syssi/esphome-treadmill-f15
// (https://github.com/syssi/esphome-treadmill-f15), Copyright syssi and
// contributors, licensed under the Apache License 2.0.
// Pinned revision 61172d6448e00d0a4eb5d70c3c39a3427c292041:
// components/treadmill_f15/treadmill_f15.{cpp,h}, components/treadmill_f15/
// button/__init__.py, components/treadmill_f15/number/ and docs/protocol.md.
// This port is part of smartfitness-bridge and distributed under
// GPL-3.0-or-later.

#include "protocols/sportstech.hpp"

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

Bytes stopFrame() { return frame(0x53, {0x03}); }

Bytes speedInclineFrame(std::uint8_t speedTenths, std::uint8_t inclinePercent) {
    return frame(0x53, {0x02, speedTenths, inclinePercent, 0, 0, 0, 0, 0, 0, 0});
}

bool decodeStatus(const std::uint8_t* data, std::size_t length, Status& out) {
    if (!data || length < 5 || length > kMaxFrameLength || data[0] != 0x02 || data[length - 1] != 0x03) return false;
    if (checksum(data + 1, length - 3) != data[length - 2]) return false;
    const std::uint8_t type = data[2];
    if (type != 0x00 && type != 0x02 && type != 0x03 && type != 0x04) return false;
    out = Status{};
    out.mode = static_cast<Mode>(type);
    if (length < 17) return true;  // stopped: 02 51 00 51 03
    auto le16 = [data](std::size_t at) {
        return static_cast<std::uint16_t>(data[at] | (static_cast<unsigned>(data[at + 1]) << 8));
    };
    out.full = true;
    out.speed = static_cast<float>(data[3]) * 0.1F;
    out.incline = static_cast<float>(data[4]) * 0.1F;
    out.elapsed = le16(5);
    out.distance = le16(7);
    out.calories = static_cast<float>(le16(9)) * 0.1F;
    out.heartRate = le16(13);
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
}

bool SportstechF15Source::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 1U) == 0) {
        setError("Sportstech notify characteristic unavailable");
        return false;
    }
    connected_ = true;
    pollMs_ = nowMs;
    return queueWrite(sportstech::kService, sportstech::kWrite, sportstech::pollFrame(), false);
}

void SportstechF15Source::tick(std::uint32_t nowMs) {
    if (!connected_ || pendingWrites() || nowMs - pollMs_ < sportstech::kPollIntervalMs) return;
    pollMs_ = nowMs;
    queueWrite(sportstech::kService, sportstech::kWrite, sportstech::pollFrame(), false);
}

void SportstechF15Source::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    sportstech::Status status;
    if (!sportstech::decodeStatus(data, length, status)) return;
    mode_ = status.mode;
    if (mode_ != Mode::Running && mode_ != Mode::Starting) speedTarget_ = inclineTarget_ = -1;
    if (!status.full) {
        // Upstream publishes speed 0 and clears the pulse for short frames;
        // totals keep their last reported values.
        sample_.speed = {0.0F, true};
        sample_.heartRate = {};
        touch(nowMs);
        return;
    }
    sample_.speed = {status.speed, true};
    sample_.incline = {status.incline, true};
    sample_.elapsed = {static_cast<float>(status.elapsed), true};
    sample_.distance = {static_cast<float>(status.distance), true};
    sample_.totalEnergy = {status.calories, true};
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
        if (mode_ != Mode::Running && mode_ != Mode::Starting) return false;
        speedTarget_ = inclineTarget_ = -1;
        return queueWrite(sportstech::kService, sportstech::kWrite, sportstech::stopFrame(), false);
    }
    // Speed/incline frames are only sent to a running belt; the bridge never
    // starts the treadmill.
    if (mode_ != Mode::Running || !sample_.speed.valid) return false;
    long speed = speedTarget_ >= 0 ? speedTarget_ : std::lround(sample_.speed.value * 10);
    long incline = inclineTarget_ >= 0 ? inclineTarget_ : (sample_.incline.valid ? std::lround(sample_.incline.value) : 0);
    if (request.control == Control::Speed) {
        if (request.value < sportstech::kMinSpeed || request.value > sportstech::kMaxSpeed) return false;
        speed = std::lround(request.value * 10);
    } else {
        if (request.value < 0.0F || request.value > sportstech::kMaxIncline) return false;
        incline = std::lround(request.value);
    }
    if (speed < std::lround(sportstech::kMinSpeed * 10) || speed > std::lround(sportstech::kMaxSpeed * 10) ||
        incline < 0 || incline > static_cast<long>(sportstech::kMaxIncline))
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
