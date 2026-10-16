// Domyos / Decathlon treadmill, elliptical and rower (Chang Yow consoles).
//
// GPL-3.0 port of qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift),
// Copyright (C) Roberto Viola and contributors. This port is part of
// smartfitness-bridge and distributed under the same license.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/domyostreadmill/domyostreadmill.{cpp,h},
// src/devices/domyoselliptical/domyoselliptical.{cpp,h},
// src/devices/domyosrower/domyosrower.{cpp,h},
// src/devices/bluetooth.cpp (advertisement rules), src/qzsettings.h (defaults).

#include "protocols/domyos_ext.hpp"

#include <cctype>
#include <cmath>
#include <string>

#include "ftms/ftms_codec.hpp"

namespace smartfitness {
namespace protocols {
namespace domyos_ext {

std::uint8_t checksum(const std::uint8_t* data, std::size_t length) {
    std::uint8_t sum = 0;
    for (std::size_t index = 0; index < length; ++index) sum = static_cast<std::uint8_t>(sum + data[index]);
    return sum;
}

std::vector<Step> initSteps(Kind kind) {
    // Common head of btinit() / btinit_changyow(); upstream labels f0 c8 01 as
    // "set speed and incline to 0" (the stop command is f0 c8 00, never sent).
    std::vector<Step> steps{
        {{0xf0, 0xc8, 0x01, 0xb9}, true}, {{0xf0, 0xc9, 0xb9}, true},       {{0xf0, 0xa3, 0x93}, true},
        {{0xf0, 0xa4, 0x94}, true},       {{0xf0, 0xa5, 0x95}, true},       {{0xf0, 0xab, 0x9b}, true},
        {{0xf0, 0xc4, 0x03, 0xb7}, true},
    };
    // The treadmill (domyos_treadmill_sync_start=false, startTape=false) stops
    // here; the elliptical and rower always send the console set-up frames.
    // A connect must never change a running belt, so the treadmill omits the
    // "set speed and incline to 0" frame (f0 c8 01) that the other kinds send.
    if (kind == Kind::Treadmill) {
        steps.erase(steps.begin());
        return steps;
    }
    steps.push_back({{0xf0, 0xad, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0xff},
                     false});
    steps.push_back({{0xff, 0xff, 0x8b}, true});  // power on bt icon
    steps.push_back({{0xf0, 0xcb, 0x02, 0x00, 0x08, 0xff, 0xff, 0xff, 0xff, 0xff,
                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00},
                     false});
    steps.push_back({{0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xb6}, true});  // power on bt word
    // Upstream sends this 20-byte head alone when startTape is false; its
    // "start tape" tail (ff ff 94) is intentionally not part of the port.
    steps.push_back({{0xf0, 0xad, 0xff, 0xff, 0x00, 0x05, 0xff, 0xff, 0xff, 0xff,
                      0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x01, 0xff},
                     false});
    return steps;
}

std::vector<Bytes> speedInclineFrames(float speedKmh, float inclinePercent) {
    Bytes frame{0xf0, 0xad, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    const auto speed = static_cast<std::uint16_t>(std::lround(speedKmh * 10.0F));
    const auto incline = static_cast<std::uint16_t>(std::lround(inclinePercent * 10.0F));
    frame[4] = static_cast<std::uint8_t>(speed >> 8);
    frame[5] = static_cast<std::uint8_t>(speed);
    frame[13] = static_cast<std::uint8_t>(incline >> 8);
    frame[14] = static_cast<std::uint8_t>(incline);
    frame[22] = checksum(frame.data(), frame.size() - 1);
    return {Bytes(frame.begin(), frame.begin() + 20), Bytes(frame.begin() + 20, frame.end())};
}

Bytes inclinationFrame(std::int8_t level) {
    Bytes frame{0xf0, 0xe3, static_cast<std::uint8_t>(level + 1), 0x00};
    frame[3] = checksum(frame.data(), 3);
    return frame;
}

std::int8_t ellipticalInclinationLevel(float percent) {
    if (percent > kMaxEllipticalIncline) percent = kMaxEllipticalIncline;
    else if (percent == 0.0F) percent = 1.0F;
    return static_cast<std::int8_t>(percent);
}

namespace {

constexpr std::size_t kFrameLength = 26;

std::uint16_t be16(const Bytes& frame, std::size_t at) {
    return static_cast<std::uint16_t>((frame[at] << 8) | frame[at + 1]);
}

int resistanceAt(const Bytes& frame) {
    const int value = static_cast<std::int8_t>(frame[14]);
    return value < 1 ? 1 : value;
}

bool startsWith(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

std::string upper(const std::string& value) {
    std::string out = value;
    for (char& character : out) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return out;
}

bool hasFtms(const ble::Advertisement& advertisement) { return advertisement.hasService(ftms::kServiceUuid); }

// Names claimed by bridges, the existing "domyos" bike entry and the other
// Domyos branches that precede the generic treadmill rule upstream.
bool reserved(const ble::Advertisement& advertisement) {
    const std::string name = upper(advertisement.name);
    return startsWith(advertisement.name, "DomyosBr") || startsWith(name, "DOMYOS-BIK");
}

}  // namespace

bool decodeTreadmill(const Bytes& frame, TreadmillStatus& out) {
    if (frame.size() != kFrameLength) return false;
    out = TreadmillStatus{};
    out.speed = static_cast<float>(frame[7]) / 10.0F;
    const std::uint16_t raw = be16(frame, 2);
    float incline = raw > 10000 ? (static_cast<float>(raw) - 65512.0F) / 10.0F : (static_cast<float>(raw) - 1000.0F) / 10.0F;
    out.incline = incline < 0.0F ? 0.0F : incline;
    out.heartRate = frame[18];
    return true;
}

bool decodeElliptical(const Bytes& frame, EllipticalStatus& out) {
    if (frame.size() != kFrameLength) return false;
    out = EllipticalStatus{};
    out.speed = static_cast<float>(be16(frame, 6)) / 10.0F;
    out.cadence = frame[9];
    out.calories = be16(frame, 10);
    out.resistance = resistanceAt(frame);
    const int incline = static_cast<std::int8_t>(frame[21]);
    out.incline = incline < 0 || incline > 15 ? 0.0F : static_cast<float>(incline);
    out.heartRate = frame[18];
    return true;
}

bool decodeRower(const Bytes& frame, RowerStatus& out) {
    if (frame.size() != kFrameLength) return false;
    out = RowerStatus{};
    out.cadence = frame[9];
    out.calories = be16(frame, 10);
    out.resistance = resistanceAt(frame);
    out.heartRate = frame[18];
    out.strokes = be16(frame, 2);
    const std::uint16_t split = be16(frame, 6);
    if (split != 0 && split <= 65000 && out.cadence > 0.0F) {
        out.pace = split;
        out.speed = (60.0F / static_cast<float>(split)) * 30.0F;
    }
    return true;
}

bool matchesTreadmill(const ble::Advertisement& advertisement) {
    const std::string name = upper(advertisement.name);
    return startsWith(advertisement.name, "Domyos") && !reserved(advertisement) && !startsWith(name, "DOMYOS-ROW") &&
           !startsWith(advertisement.name, "Domyos-EL") && !hasFtms(advertisement);
}

bool matchesElliptical(const ble::Advertisement& advertisement) {
    return startsWith(advertisement.name, "Domyos-EL") && !reserved(advertisement) && !hasFtms(advertisement);
}

bool matchesRower(const ble::Advertisement& advertisement) {
    return startsWith(upper(advertisement.name), "DOMYOS-ROW") && !startsWith(advertisement.name, "DomyosBridge") &&
           !hasFtms(advertisement);
}

namespace {

template <Kind K>
std::unique_ptr<SourceProtocol> make() {
    return std::unique_ptr<SourceProtocol>(new DomyosExtSource(K));
}

// Append only: the order is the persisted pairing/detection tie order.
const ProtocolInfo kDescriptors[] = {
    {"domyos-tm", "Domyos / Decathlon treadmill", kMatchPriority, matchesTreadmill, make<Kind::Treadmill>, true, false,
     MachineProfile::Treadmill},
    {"domyos-el", "Domyos / Decathlon elliptical", kMatchPriority, matchesElliptical, make<Kind::Elliptical>, true,
     false, MachineProfile::CrossTrainer},
    {"domyos-row", "Domyos / Decathlon rower", kMatchPriority, matchesRower, make<Kind::Rower>, true, false,
     MachineProfile::Rower},
};

}  // namespace

std::size_t descriptorCount() { return sizeof(kDescriptors) / sizeof(kDescriptors[0]); }
const ProtocolInfo& descriptorAt(std::size_t index) {
    return kDescriptors[index < descriptorCount() ? index : 0];
}

}  // namespace domyos_ext

using domyos_ext::Kind;

const char* DomyosExtSource::id() const {
    switch (kind_) {
        case Kind::Elliptical: return "domyos-el";
        case Kind::Rower: return "domyos-row";
        default: return "domyos-tm";
    }
}

MachineProfile DomyosExtSource::profile() const {
    switch (kind_) {
        case Kind::Elliptical: return MachineProfile::CrossTrainer;
        case Kind::Rower: return MachineProfile::Rower;
        default: return MachineProfile::Treadmill;
    }
}

ble::ConnectPlan DomyosExtSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(domyos::kService, domyos::kNotify);
    return plan;
}

void DomyosExtSource::reset() {
    SourceProtocol::reset();
    pending_.clear();
    assembler_.reset();
    sentMs_ = pollMs_ = 0;
    speedTarget_ = inclineTarget_ = -1.0F;
    connected_ = initQueued_ = initDone_ = waiting_ = false;
}

bool DomyosExtSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    if ((subscribedMask & 1U) == 0) {
        setError("Domyos notify characteristic unavailable");
        return false;
    }
    connected_ = true;
    pending_.clear();
    waiting_ = false;
    initQueued_ = enqueue(domyos_ext::initSteps(kind_), nowMs);
    pollMs_ = nowMs;
    return initQueued_;
}

bool DomyosExtSource::enqueue(const std::vector<domyos_ext::Step>& steps, std::uint32_t nowMs) {
    if (!connected_ || pending_.size() + steps.size() > kMaxPending) return false;
    pending_.insert(pending_.end(), steps.begin(), steps.end());
    flush(nowMs);
    return true;
}

void DomyosExtSource::flush(std::uint32_t nowMs) {
    // The treadmill pauses writes while a split status frame is incomplete.
    if (kind_ == Kind::Treadmill && assembler_.incomplete()) return;
    std::size_t sent = 0;
    while (!waiting_ && sent < pending_.size()) {
        const auto& step = pending_[sent];
        if (!queueWrite(domyos::kService, domyos::kWrite, step.bytes)) break;
        ++sent;
        if (step.wait) {
            waiting_ = true;
            sentMs_ = nowMs;
        }
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(sent));
    if (initQueued_ && pending_.empty()) initDone_ = true;
}

void DomyosExtSource::tick(std::uint32_t nowMs) {
    if (!connected_) return;
    if (waiting_ && nowMs - sentMs_ >= domyos_ext::kWriteTimeoutMs) waiting_ = false;
    flush(nowMs);
    const std::uint32_t interval = kind_ == Kind::Treadmill ? domyos_ext::kTreadmillPollMs : domyos_ext::kPollMs;
    if (!ready() || waiting_ || !pending_.empty() || nowMs - pollMs_ < interval) return;
    if (kind_ == Kind::Treadmill && assembler_.incomplete()) return;
    pollMs_ = nowMs;
    // Upstream also refreshes the console display about once per second; this
    // port only keeps the console streaming with the no-op poll.
    enqueue({{domyos::noOpFrame(), true}}, nowMs);
}

void DomyosExtSource::onNotify(std::size_t, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (!data) return;
    if (length != 20) waiting_ = false;  // upstream packetReceived()
    domyos_ext::Bytes frame;
    if (kind_ == Kind::Treadmill) {
        if (!assembler_.feed(data, length, frame)) {
            flush(nowMs);
            return;
        }
    } else {
        frame.assign(data, data + length);
    }

    if (kind_ == Kind::Treadmill) {
        domyos_ext::TreadmillStatus status;
        if (domyos_ext::decodeTreadmill(frame, status)) {
            // Console buttons (byte 22) are ignored: upstream would start or
            // stop the belt from them.
            sample_.speed = {status.speed, true};
            sample_.incline = {status.incline, true};
            sample_.heartRate = status.heartRate > 0 ? MetricSample{static_cast<float>(status.heartRate), true}
                                                     : MetricSample{};
            if (status.speed <= 0.0F) speedTarget_ = inclineTarget_ = -1.0F;
            if (speedTarget_ >= 0.0F && std::fabs(status.speed - speedTarget_) < 0.05F) speedTarget_ = -1.0F;
            if (inclineTarget_ >= 0.0F && std::fabs(status.incline - inclineTarget_) < 0.05F) inclineTarget_ = -1.0F;
            touch(nowMs);
        }
    } else if (kind_ == Kind::Elliptical) {
        domyos_ext::EllipticalStatus status;
        if (domyos_ext::decodeElliptical(frame, status)) {
            sample_.speed = {status.speed, true};
            sample_.cadence = {status.cadence, true};
            sample_.resistance = {static_cast<float>(status.resistance), true};
            sample_.incline = {status.incline, true};
            sample_.totalEnergy = {status.calories, true};
            sample_.heartRate = status.heartRate > 0 ? MetricSample{static_cast<float>(status.heartRate), true}
                                                     : MetricSample{};
            touch(nowMs);
        }
    } else {
        domyos_ext::RowerStatus status;
        if (domyos_ext::decodeRower(frame, status)) {
            sample_.speed = {status.speed, true};
            sample_.pace = {status.pace, true};
            sample_.cadence = {status.cadence, true};
            sample_.resistance = {static_cast<float>(status.resistance), true};
            sample_.totalEnergy = {status.calories, true};
            sample_.strokeCount = {status.strokes, true};
            sample_.heartRate = status.heartRate > 0 ? MetricSample{static_cast<float>(status.heartRate), true}
                                                     : MetricSample{};
            touch(nowMs);
        }
    }
    flush(nowMs);
}

bool DomyosExtSource::treadmillControl(const ControlRequest& request, std::uint32_t nowMs) {
    // Only a belt reported running with fresh data is adjusted; the bridge
    // never starts or stops it (speed 0 is rejected rather than sent).
    if (!hasData() || nowMs - lastDataMs() > domyos_ext::kFreshMs || !sample_.speed.valid ||
        sample_.speed.value <= 0.0F)
        return false;
    float speed = speedTarget_ >= 0.0F ? speedTarget_ : sample_.speed.value;
    float incline = inclineTarget_ >= 0.0F ? inclineTarget_ : (sample_.incline.valid ? sample_.incline.value : 0.0F);
    if (request.control == Control::Speed) {
        if (request.value <= 0.0F || request.value > domyos_ext::kMaxSpeed) return false;
        speed = std::round(request.value * 10.0F) / 10.0F;
        if (speed <= 0.0F) return false;
    } else {
        // Upstream clamps negative grades to 0 and uses 0.5 % steps.
        incline = std::round((request.value < 0.0F ? 0.0F : request.value) * 2.0F) / 2.0F;
        if (incline > domyos_ext::kMaxIncline) return false;
    }
    const auto frames = domyos_ext::speedInclineFrames(speed, incline);
    if (!enqueue({{frames[0], false}, {frames[1], true}}, nowMs)) return false;
    if (request.control == Control::Speed) speedTarget_ = speed;
    else inclineTarget_ = incline;
    return true;
}

bool DomyosExtSource::control(const ControlRequest& request, std::uint32_t nowMs) {
    if (!ready() || failed() || !std::isfinite(request.value) || !capabilities().supports(request.control)) return false;
    if (kind_ == Kind::Treadmill) return treadmillControl(request, nowMs);
    if (request.control == Control::Resistance) {
        const int level = percentToLevel(request.value, domyos_ext::kMinResistance, domyos_ext::kMaxResistance);
        const auto frames = domyos::resistanceFrames(static_cast<std::uint8_t>(level));
        return enqueue({{frames[0], false}, {frames[1], false}}, nowMs);
    }
    // Elliptical inclination (upstream domyos_elliptical_inclination default).
    if (request.value < 0.0F) return false;
    return enqueue({{domyos_ext::inclinationFrame(domyos_ext::ellipticalInclinationLevel(request.value)), false}},
                   nowMs);
}

TrainerCapabilities DomyosExtSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    const bool controls = ready() && !failed();
    switch (kind_) {
        case Kind::Treadmill:
            capabilities.setControl(Control::Speed, controls && hasData());
            capabilities.setControl(Control::Incline, controls && hasData());
            break;
        case Kind::Elliptical:
            capabilities.setControl(Control::Resistance, controls);
            capabilities.setControl(Control::Incline, controls);
            break;
        case Kind::Rower:
            capabilities.setControl(Control::Resistance, controls);
            break;
    }
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
