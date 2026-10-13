// GPL-3.0, Copyright syssi and esphome-treadmill-f15 contributors.
// Ported from https://github.com/syssi/esphome-treadmill-f15 at
// 61172d6448e00d0a4eb5d70c3c39a3427c292041:
// components/treadmill_f15/treadmill_f15.{cpp,h},
// components/treadmill_f15/{button,number}/.

#include "protocols/sportstech_f15.hpp"

#include <algorithm>
#include <cmath>

namespace smartfitness {
namespace protocols {
namespace {
using ble::Bytes;
const ble::Uuid service = ble::Uuid::from16(0xfff0);
const ble::Uuid notifyCharacteristic = ble::Uuid::from16(0xfff1);
const ble::Uuid writeCharacteristic = ble::Uuid::from16(0xfff2);
const Bytes poll{0x02, 0x51, 0x51, 0x03};
const Bytes start{0x02, 0x53, 0x01, 0, 0, 0, 0, 0, 0x52, 0x03};
const Bytes stop{0x02, 0x53, 0x03, 0x50, 0x03};

std::uint8_t checksum(const Bytes& frame, std::size_t length) {
    std::uint8_t value = 0;
    for (std::size_t i = 1; i + 2 < length; ++i) value ^= frame[i];
    return value;
}
bool valid(const Bytes& frame, std::size_t length) {
    return frame.size() >= length && frame[0] == 2 && frame[1] == 0x51 &&
        frame[length - 1] == 3 && frame[length - 2] == checksum(frame, length);
}
unsigned le16(const Bytes& frame, std::size_t at) {
    return frame[at] | (static_cast<unsigned>(frame[at + 1]) << 8);
}
}  // namespace

bool matchesSportstechF15(const ble::Advertisement& advertisement) {
    if (advertisement.hasService(0x1826)) return false;
    std::string name = advertisement.name;
    for (char& c : name) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const bool f15Name = name == "SPORTSTECH F15" || name.compare(0, 15, "SPORTSTECH F15 ") == 0 ||
        name.compare(0, 15, "SPORTSTECH F15-") == 0;
    // FFF0 is shared by many unrelated vendors; a name is always required.
    return name.compare(0, 3, "FS-") == 0 || f15Name ||
        (advertisement.hasService(service) && (name == "F15" || name.compare(0, 4, "F15-") == 0));
}

ble::ConnectPlan SportstechF15Treadmill::plan() const {
    ble::ConnectPlan result;
    result.subscribe(service, notifyCharacteristic);
    result.writeEndpoints[0] = {service, writeCharacteristic, false};
    return result;
}
void SportstechF15Treadmill::onWriteEndpoint(const ble::Uuid& s, const ble::Uuid& c) {
    if (s == service && c == writeCharacteristic) writeFound_ = true;
}
bool SportstechF15Treadmill::onConnected(std::uint32_t mask, std::uint32_t nowMs) {
    connected_ = (mask & 1) && writeFound_ && !failed();
    if (!connected_) {
        setError("F15 requires FFF1 notifications and FFF2 write endpoint");
        return false;
    }
    pollMs_ = nowMs;
    return send(poll);
}
bool SportstechF15Treadmill::send(const Bytes& bytes) {
    if (!connected_ || failed()) return false;
    if (queueWrite(service, writeCharacteristic, bytes, false)) return true;
    delayedStart_ = false;
    return false;
}
bool SportstechF15Treadmill::sendTarget(float speed, float incline) {
    Bytes bytes{2, 0x53, 2, static_cast<std::uint8_t>(std::lround(speed * 10)),
                static_cast<std::uint8_t>(incline), 0, 0, 0, 0, 0, 0, 0, 0, 3};
    bytes[12] = checksum(bytes, bytes.size());
    return send(bytes);
}
bool SportstechF15Treadmill::fresh(std::uint32_t nowMs) const {
    return connected_ && usable_ && !failed() && hasData() &&
        static_cast<std::uint32_t>(nowMs - lastDataMs()) <= kFreshStatusMs;
}
void SportstechF15Treadmill::onNotify(std::size_t index, const std::uint8_t* data,
                                    std::size_t length, std::uint32_t nowMs) {
    if (index != 0 || !connected_ || failed() || !data || !length) return;
    if (!receive_.empty() && static_cast<std::uint32_t>(nowMs - receiveMs_) > 1000) receive_.clear();
    receiveMs_ = nowMs;
    for (std::size_t i = 0; i < length; ++i) {
        if (receive_.size() == 108) {
            parse(nowMs, false);
            if (receive_.size() == 108) receive_.clear();
        }
        receive_.push_back(data[i]);
    }
    parse(nowMs, false);
}
void SportstechF15Treadmill::parse(std::uint32_t nowMs, bool flushShort) {
    while (!receive_.empty()) {
        if (receive_[0] != 2 || (receive_.size() >= 2 && receive_[1] != 0x51) ||
            (receive_.size() >= 3 && receive_[2] > 4)) {
            receive_.erase(receive_.begin());
            continue;
        }
        if (receive_.size() < 5) return;
        const auto state = receive_[2];
        const std::size_t shortLength = state == 1 || state == 2 ? 6 : 5;
        std::size_t length = 0;
        if (state != 1 && valid(receive_, 17)) length = 17;
        // Short frames have no length field. Wait briefly for fragmented full
        // status instead of mistaking a payload 03 for an end marker.
        else if (valid(receive_, shortLength) &&
                 (state == 1 || flushShort ||
                  (receive_.size() > shortLength + 1 && receive_[shortLength] == 2 &&
                   receive_[shortLength + 1] == 0x51))) length = shortLength;
        if (!length) {
            if (receive_.size() >= 17 || (state == 1 && receive_.size() >= 6)) {
                receive_.erase(receive_.begin());
                continue;
            }
            return;
        }
        accept(Bytes(receive_.begin(), receive_.begin() + length), nowMs);
        receive_.erase(receive_.begin(), receive_.begin() + length);
    }
}
void SportstechF15Treadmill::accept(const Bytes& frame, std::uint32_t nowMs) {
    if (frame[2] == 1) return;  // Start ACK is not telemetry.
    operation_ = static_cast<TreadmillOperation>(frame[2]);
    if (delayedStart_ && (operation_ == TreadmillOperation::Starting || operation_ == TreadmillOperation::Running))
        startupObserved_ = true;
    if (operation_ == TreadmillOperation::Stopping ||
        (operation_ == TreadmillOperation::Stopped && startupObserved_))
        delayedStart_ = false;
    if (frame.size() == 17) {
        sample_.speed = {frame[3] * 0.1F, true};
        // Upstream publishes tenths although its log labels this as integer %.
        sample_.incline = {frame[4] * 0.1F, true};
        sample_.elapsed = {static_cast<float>(le16(frame, 5)), true};
        sample_.distance = {static_cast<float>(le16(frame, 7)), true};
        sample_.totalEnergy = {le16(frame, 9) * 0.1F, true};
        sample_.heartRate = {static_cast<float>(le16(frame, 13)), true};
    } else {
        sample_.speed = {0, true};
        sample_.heartRate = {0, true};
    }
    usable_ = true;
    touch(nowMs);
}
void SportstechF15Treadmill::tick(std::uint32_t nowMs) {
    if (!connected_ || failed()) return;
    if (!receive_.empty() && static_cast<std::uint32_t>(nowMs - receiveMs_) >= 1000) parse(nowMs, true);
    if (hasData() && static_cast<std::uint32_t>(nowMs - lastDataMs()) > kFreshStatusMs) {
        usable_ = false;
        delayedStart_ = false;
    }
    if (delayedStart_ && static_cast<std::uint32_t>(nowMs - startMs_) >= 6000) {
        delayedStart_ = false;
        if (fresh(nowMs) && startupObserved_ &&
            (operation_ == TreadmillOperation::Starting || operation_ == TreadmillOperation::Running))
            sendTarget(speedTarget_, inclineTarget_);
    }
    if (static_cast<std::uint32_t>(nowMs - pollMs_) >= 1000) {
        pollMs_ = nowMs;
        send(poll);
    }
}
TrainerCapabilities SportstechF15Treadmill::capabilities() const {
    TrainerCapabilities result;
    result.metrics = observedMetrics();
    const bool enabled = connected_ && usable_ && !failed();
    result.setControl(Control::Speed, enabled);
    result.setControl(Control::Incline, enabled);
    result.setControl(Control::RunState, enabled);
    return result;
}
void SportstechF15Treadmill::discardWrites() {
    ble::GattWrite write;
    while (takeWrite(write)) {}
}
bool SportstechF15Treadmill::control(const ControlRequest& request, std::uint32_t nowMs) {
    if (!fresh(nowMs) || !capabilities().supports(request.control) || !std::isfinite(request.value)) return false;
    if ((request.control == Control::Speed && request.value == 0) ||
        (request.control == Control::RunState && (request.value == 0 || request.value == 2))) {
        // F15 has no native pause: pause explicitly uses the hardware stop.
        delayedStart_ = false;
        startupObserved_ = false;
        discardWrites();
        return send(stop);
    }
    if (request.control == Control::RunState) {
        if (request.value != 1 || delayedStart_ || operation_ == TreadmillOperation::Starting ||
            operation_ == TreadmillOperation::Running || operation_ == TreadmillOperation::Stopping) return false;
        if (!send(start)) return false;
        delayedStart_ = true;
        startupObserved_ = false;
        startMs_ = nowMs;
        return true;
    }
    if (delayedStart_ || operation_ == TreadmillOperation::Starting ||
        operation_ == TreadmillOperation::Stopping) return false;
    float speed = speedTarget_, incline = inclineTarget_;
    if (request.control == Control::Speed) {
        if (request.value < 0.8F || request.value > 12) return false;
        speed = std::round(request.value * 10) / 10;
    } else if (request.control == Control::Incline) {
        if (request.value < 0 || request.value > 15) return false;
        incline = std::trunc(request.value);
    } else return false;
    if (!sendTarget(speed, incline)) return false;
    speedTarget_ = speed;
    inclineTarget_ = incline;
    return true;
}
void SportstechF15Treadmill::onWriteFailed(std::uint32_t) {
    delayedStart_ = false;
    startupObserved_ = false;
    usable_ = false;
    discardWrites();
    fail("F15 write failed");
}
void SportstechF15Treadmill::reset() {
    SourceProtocol::reset();
    receive_.clear();
    operation_.reset();
    receiveMs_ = pollMs_ = startMs_ = 0;
    speedTarget_ = 0.8F;
    inclineTarget_ = 0;
    writeFound_ = connected_ = usable_ = delayedStart_ = false;
    startupObserved_ = false;
}

}  // namespace protocols
}  // namespace smartfitness
