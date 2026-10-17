#include "protocols/ftms_source.hpp"
#include "debug_log.h"

#include <cmath>
#include <cstdio>

namespace smartfitness {
namespace protocols {
namespace {

ble::Uuid uuid16(std::uint16_t value) { return ble::Uuid::from16(value); }
const ble::Uuid kService = ble::Uuid::from16(ftms::kServiceUuid);
const ble::Uuid kControlPoint = ble::Uuid::from16(ftms::kControlPointUuid);

MachineProfile subscriptionProfile(std::size_t index) {
    switch (index) {
        case FtmsSource::SubTreadmill: return MachineProfile::Treadmill;
        case FtmsSource::SubCrossTrainer: return MachineProfile::CrossTrainer;
        case FtmsSource::SubRower: return MachineProfile::Rower;
        case FtmsSource::SubStepClimber: return MachineProfile::StepClimber;
        case FtmsSource::SubStairClimber: return MachineProfile::StairClimber;
        default: return MachineProfile::IndoorBike;
    }
}

bool isDataSubscription(std::size_t index) {
    return index < FtmsSource::SubCount && (FtmsSource::kDataSubscriptions & (1U << index)) != 0;
}

unsigned machineType(std::size_t subscription) {
    switch (subscription) {
        case FtmsSource::SubBike: return 1U << 5;
        case FtmsSource::SubTreadmill: return 1U << 0;
        case FtmsSource::SubCrossTrainer: return 1U << 1;
        case FtmsSource::SubStepClimber: return 1U << 2;
        case FtmsSource::SubStairClimber: return 1U << 3;
        case FtmsSource::SubRower: return 1U << 4;
        default: return 0;
    }
}

const char* resultName(ftms::ResultCode code) {
    switch (code) {
        case ftms::ResultCode::Success: return "success";
        case ftms::ResultCode::NotSupported: return "op code not supported";
        case ftms::ResultCode::InvalidParameter: return "invalid parameter";
        case ftms::ResultCode::OperationFailed: return "operation failed";
        case ftms::ResultCode::ControlNotPermitted: return "control not permitted";
    }
    return "unknown result";
}

#if defined(ARDUINO) && defined(DEBUG_WEB)
const char* opcodeName(std::uint8_t opcode) {
    switch (static_cast<ftms::OpCode>(opcode)) {
        case ftms::OpCode::RequestControl: return "Request Control";
        case ftms::OpCode::Reset: return "Reset";
        case ftms::OpCode::SetTargetSpeed: return "Set Target Speed";
        case ftms::OpCode::SetTargetInclination: return "Set Target Inclination";
        case ftms::OpCode::SetTargetResistance: return "Set Target Resistance";
        case ftms::OpCode::SetTargetPower: return "Set Target Power";
        case ftms::OpCode::StartOrResume: return "Start/Resume";
        case ftms::OpCode::StopOrPause: return "Stop/Pause";
        case ftms::OpCode::SetIndoorBikeSimulation: return "Indoor Bike Simulation";
        case ftms::OpCode::ResponseCode: return "Response";
    }
    return "unknown";
}

const char* controlStateName(FtmsSource::ControlState state) {
    switch (state) {
        case FtmsSource::ControlState::Unavailable: return "unavailable";
        case FtmsSource::ControlState::Requesting: return "requesting";
        case FtmsSource::ControlState::Granted: return "granted";
        case FtmsSource::ControlState::Denied: return "denied";
    }
    return "unknown";
}

void logResponse(const std::uint8_t* data, std::size_t length) {
    ftms::ControlPointResponse response;
    if (!ftms::decodeControlPointResponse(data, length, response)) {
        SF_DEBUG_LOG("[FTMS-CP] response malformed decode=failed (truncated/invalid payload) len=%u raw=%s\n",
                     static_cast<unsigned>(length), debuglog::Hex(data, data ? length : 0).c_str());
        return;
    }
    SF_DEBUG_LOG("[FTMS-CP] response to %s (0x%02x): %s result=0x%02x\n",
                 opcodeName(response.requestOpcode), static_cast<unsigned>(response.requestOpcode),
                 resultName(response.result), static_cast<unsigned>(response.result));
}

void logMachineData(std::size_t identity, const std::uint8_t* data, std::size_t length,
                    const ftms::MachineData& decoded, bool ok) {
    const auto* name = machineProfileName(subscriptionProfile(identity));
    const std::size_t flagBytes = identity == FtmsSource::SubCrossTrainer ? 3 : 2;
    if (!data || length < flagBytes) {
        SF_DEBUG_LOG("[FTMS-IN] %s decode=failed: truncated/invalid payload (flags missing) len=%u\n",
                     name, static_cast<unsigned>(length));
        return;
    }
    unsigned flags = data[0] | (static_cast<unsigned>(data[1]) << 8);
    if (flagBytes == 3) flags |= static_cast<unsigned>(data[2]) << 16;
    if (!ok) {
        SF_DEBUG_LOG("[FTMS-IN] %s flags=0x%06x more=%u len=%u decode=failed: truncated/invalid payload\n",
                     name, flags, flags & 1U, static_cast<unsigned>(length));
        return;
    }
    const char* direction = identity == FtmsSource::SubCrossTrainer
        ? (flags & (1U << 15) ? " movementBackward=1" : " movementBackward=0") : "";
    char line[1024];
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        line, sizeof(line), "[FTMS-IN] %s flags=0x%06x more=%u len=%u%s",
        name, flags, flags & 1U, static_cast<unsigned>(length), direction));
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        const auto metric = static_cast<Metric>(index);
        if (!decoded[metric].valid) continue;
        // Reserve the newline and terminator within DebugSerial.printf's 1024-byte limit.
        const std::size_t available = sizeof(line) - used - 1;
        int count = std::snprintf(line + used, available, " %s=%.2f", metricKey(metric),
                                  static_cast<double>(decoded[metric].value));
        if (static_cast<std::size_t>(count) >= available) {
            line[used] = '\0';
            SF_DEBUG_LOG("%s\n", line);
            used = static_cast<std::size_t>(std::snprintf(
                line, sizeof(line), "[FTMS-IN] %s continuation flags=0x%06x more=%u", name, flags, flags & 1U));
            count = std::snprintf(line + used, sizeof(line) - used - 1, " %s=%.2f", metricKey(metric),
                                  static_cast<double>(decoded[metric].value));
        }
        used += static_cast<std::size_t>(count);
    }
    SF_DEBUG_LOG("%s\n", line);
}
#endif

}  // namespace

FtmsSource::FtmsSource() : FtmsSource(Options{}) {}
FtmsSource::FtmsSource(const Options& options) : options_(options) {}

ble::ConnectPlan FtmsSource::plan() const {
    ble::ConnectPlan plan;
    subscriptionCount_ = 0;
    const std::uint16_t characteristics[SubCount] = {
        ftms::kIndoorBikeDataUuid, ftms::kTreadmillDataUuid, ftms::kCrossTrainerDataUuid,
        ftms::kRowerDataUuid, ftms::kControlPointUuid, ftms::kStepClimberDataUuid, ftms::kStairClimberDataUuid
#if defined(ARDUINO) && defined(DEBUG_WEB)
        , ftms::kMachineStatusUuid
#endif
    };
    for (std::size_t identity = SubBike; identity < SubCount; ++identity) {
        if (options_.bikeDataOnly && identity != SubBike) continue;
        const bool data = isDataSubscription(identity);
        bool optional = data && !options_.bikeDataOnly && !lastMachineTypes_;
#if defined(ARDUINO) && defined(DEBUG_WEB)
        if (identity == SubMachineStatus) optional = true;
#endif
        if (data && !options_.bikeDataOnly && lastMachineTypes_ &&
            !(lastMachineTypes_ & machineType(identity))) continue;
        subscriptions_[subscriptionCount_++] = static_cast<Subscription>(identity);
        plan.subscribe(kService, uuid16(characteristics[identity]), identity == SubControlPoint,
                       optional);
    }
    if (options_.bikeDataOnly) return plan;
    plan.read(kService, uuid16(ftms::kFeatureUuid));
    plan.read(kService, uuid16(ftms::kSupportedResistanceRangeUuid));
    plan.read(kService, uuid16(ftms::kSupportedInclinationRangeUuid));
    plan.read(kService, uuid16(ftms::kSupportedPowerRangeUuid));
    return plan;
}

FtmsSource::Subscription FtmsSource::subscriptionAt(std::size_t index) const {
    return index < subscriptionCount_ ? subscriptions_[index] : SubCount;
}

void FtmsSource::reset() {
    SourceProtocol::reset();
    profile_ = MachineProfile::IndoorBike;
    advertisedProfileValid_ = false;
    subscriptionCount_ = options_.bikeDataOnly ? 1 : SubCount;
    for (std::size_t index = 0; index < SubCount; ++index)
        subscriptions_[index] = static_cast<Subscription>(index);
    dataSubscribedMask_ = 0;
    dataProfileSelected_ = false;
    features_ = {};
    featuresRead_ = false;
    resistanceRange_ = {};
    inclinationRange_ = {};
    powerRange_ = {};
    controlPointSubscribed_ = false;
    setControlState(ControlState::Unavailable);
    controlRequestedMs_ = 0;
    controlAttempts_ = 0;
}

void FtmsSource::onAdvertisement(const ble::Advertisement& advertisement, std::uint32_t) {
    if (options_.bikeDataOnly || advertisement.ftmsServiceData.size() < 3 ||
        !(advertisement.ftmsServiceData[0] & 1U)) return;
    const unsigned types = advertisement.ftmsServiceData[1] |
                           (static_cast<unsigned>(advertisement.ftmsServiceData[2]) << 8);
    // FTMS service data: flags, then the little-endian Fitness Machine Type.
    const unsigned supported = types & 0x3FU;
    if (!supported) return;
    lastMachineTypes_ = static_cast<std::uint8_t>(supported);
    advertisedProfileValid_ = (supported & (supported - 1U)) == 0;
    if (!advertisedProfileValid_) return;
    advertisedProfile_ = supported & (1U << 0) ? MachineProfile::Treadmill :
                         supported & (1U << 1) ? MachineProfile::CrossTrainer :
                         supported & (1U << 2) ? MachineProfile::StepClimber :
                         supported & (1U << 3) ? MachineProfile::StairClimber :
                         supported & (1U << 4) ? MachineProfile::Rower : MachineProfile::IndoorBike;
    for (std::size_t index = SubBike; index < SubCount; ++index) {
        if (isDataSubscription(index) && (dataSubscribedMask_ & (1U << index)) &&
            subscriptionProfile(index) == advertisedProfile_) {
            if (profile_ != advertisedProfile_) clearSample();
            profile_ = advertisedProfile_;
            dataProfileSelected_ = true;
            return;
        }
    }
    if (!dataSubscribedMask_) profile_ = advertisedProfile_;
}

void FtmsSource::onRead(std::size_t index, const std::uint8_t* data, std::size_t length) {
    switch (index) {
        case ReadFeature: featuresRead_ = ftms::decodeFeature(data, length, features_); break;
        case ReadResistanceRange: ftms::decodeSupportedRange(data, length, 0.1F, resistanceRange_); break;
        case ReadInclinationRange: ftms::decodeSupportedRange(data, length, 0.1F, inclinationRange_); break;
        case ReadPowerRange: ftms::decodeSupportedRange(data, length, 1.0F, powerRange_); break;
        default: break;
    }
}

bool FtmsSource::onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) {
    std::uint32_t identities = 0;
    for (std::size_t index = 0; index < subscriptionCount_; ++index)
        if (subscribedMask & (1U << index)) identities |= 1U << subscriptionAt(index);
    if ((identities & kDataSubscriptions) == 0) {
        setError("no FTMS data characteristic");
        return false;
    }
    dataSubscribedMask_ = identities & kDataSubscriptions;
    if (options_.bikeDataOnly) dataSubscribedMask_ &= 1U << SubBike;
    if (!dataSubscribedMask_) {
        setError("no FTMS bike data characteristic");
        return false;
    }
    dataProfileSelected_ = (dataSubscribedMask_ & (dataSubscribedMask_ - 1U)) == 0;
    for (std::size_t index = SubBike; index < SubCount; ++index) {
        if (isDataSubscription(index) && (dataSubscribedMask_ & (1U << index))) {
            profile_ = subscriptionProfile(index);
            break;
        }
    }
    if (advertisedProfileValid_) {
        for (std::size_t index = SubBike; index < SubCount; ++index)
            if (isDataSubscription(index) && (dataSubscribedMask_ & (1U << index)) &&
                subscriptionProfile(index) == advertisedProfile_) {
                profile_ = advertisedProfile_;
                dataProfileSelected_ = true;
            }
    }
    controlPointSubscribed_ = options_.allowControl && !options_.bikeDataOnly &&
                              (identities & (1U << SubControlPoint)) != 0;
    const std::uint32_t controllable = ftms::kTargetPower | ftms::kTargetResistance | ftms::kTargetInclination |
                                       ftms::kTargetIndoorBikeSimulation | ftms::kTargetSpeed;
    if (controlPointSubscribed_ && featuresRead_ && (features_.targets & controllable)) requestControl(nowMs);
    return true;
}

void FtmsSource::requestControl(std::uint32_t nowMs) {
    setControlState(ControlState::Requesting);
    controlRequestedMs_ = nowMs;
    ++controlAttempts_;
    SF_DEBUG_LOG("[FTMS-CP] Request Control attempt=%u/%u\n",
                 static_cast<unsigned>(controlAttempts_), static_cast<unsigned>(kControlAttempts));
    sendControl(ftms::encodeRequestControl());
}

void FtmsSource::setControlState(ControlState state) {
    if (controlState_ != state)
        SF_DEBUG_LOG("[FTMS-CP] state %s -> %s\n", controlStateName(controlState_), controlStateName(state));
    controlState_ = state;
}

bool FtmsSource::sendControl(const ftms::Bytes& payload) {
    const bool queued = queueWrite(kService, kControlPoint, payload, true);
#if defined(ARDUINO) && defined(DEBUG_WEB)
    if (!payload.empty())
        SF_DEBUG_LOG("[FTMS-CP] send %s (0x%02x) params=%s %s\n", opcodeName(payload[0]),
                     static_cast<unsigned>(payload[0]), debuglog::Hex(payload.data() + 1, payload.size() - 1).c_str(),
                     queued ? "queued" : "dropped: write queue full/invalid payload");
#endif
    return queued;
}

void FtmsSource::tick(std::uint32_t nowMs) {
    if (controlState_ != ControlState::Requesting || nowMs - controlRequestedMs_ < kControlTimeoutMs) return;
    SF_DEBUG_LOG("[FTMS-CP] timeout Request Control attempt=%u/%u %s\n",
                 static_cast<unsigned>(controlAttempts_), static_cast<unsigned>(kControlAttempts),
                 controlAttempts_ < kControlAttempts ? "retry" : "exhausted");
    if (controlAttempts_ < kControlAttempts) {
        requestControl(nowMs);
        return;
    }
    setControlState(ControlState::Denied);
    setError("no response to FTMS Request Control");
}

void FtmsSource::handleResponse(const std::uint8_t* data, std::size_t length) {
    ftms::ControlPointResponse response;
    if (!ftms::decodeControlPointResponse(data, length, response)) return;
    if (response.requestOpcode == static_cast<std::uint8_t>(ftms::OpCode::RequestControl)) {
        if (controlState_ != ControlState::Requesting) {
            SF_DEBUG_LOG("[FTMS-CP] response ignored: not requesting control\n");
            return;
        }
        if (response.result == ftms::ResultCode::Success) {
            setControlState(ControlState::Granted);
            const bool confirmedBike = options_.bikeDataOnly || dataSubscribedMask_ == (1U << SubBike) ||
                                       (advertisedProfileValid_ && advertisedProfile_ == MachineProfile::IndoorBike);
            if (profile_ == MachineProfile::IndoorBike && confirmedBike)
                sendControl(ftms::encodeStartOrResume());
        } else {
            setControlState(ControlState::Denied);
            setError(std::string("FTMS Request Control refused: ") + resultName(response.result));
        }
        return;
    }
    if (response.result != ftms::ResultCode::Success) {
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "FTMS op 0x%02x rejected: %s", response.requestOpcode,
                      resultName(response.result));
        setError(buffer);
    }
}

void FtmsSource::apply(const ftms::MachineData& data, std::uint32_t nowMs) {
    // Devices may split one record over several notifications ("More Data"),
    // so only the fields present in this notification are updated.
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        const auto metric = static_cast<Metric>(index);
        const auto& value = data[metric];
        if (!value.valid) continue;
        // Some machines send 0 bpm when no heart rate sensor is paired.
        if (metric == Metric::HeartRate && value.value <= 0.0F) continue;
        sample_[metric] = value;
    }
    touch(nowMs);
}

void FtmsSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    const auto identity = subscriptionAt(index);
    ftms::MachineData decoded;
    bool ok = false;
    switch (identity) {
        case SubBike: ok = ftms::decodeIndoorBikeData(data, length, decoded); break;
        case SubTreadmill: ok = ftms::decodeTreadmillData(data, length, decoded); break;
        case SubCrossTrainer: ok = ftms::decodeCrossTrainerData(data, length, decoded); break;
        case SubRower: ok = ftms::decodeRowerData(data, length, decoded); break;
        case SubStepClimber: ok = ftms::decodeStepClimberData(data, length, decoded); break;
        case SubStairClimber: ok = ftms::decodeStairClimberData(data, length, decoded); break;
        case SubControlPoint:
#if defined(ARDUINO) && defined(DEBUG_WEB)
            logResponse(data, length);
#endif
            if (options_.allowControl) handleResponse(data, length);
            else SF_DEBUG_LOG("[FTMS-CP] response ignored: control disabled\n");
            return;
#if defined(ARDUINO) && defined(DEBUG_WEB)
        case SubMachineStatus:
            return;
#endif
        default:
            SF_DEBUG_LOG("[FTMS-IN] dropped: unknown subscription index=%u\n", static_cast<unsigned>(index));
            return;
    }
#if defined(ARDUINO) && defined(DEBUG_WEB)
    logMachineData(identity, data, length, decoded, ok);
#endif
    if (ok) {
        if (options_.bikeDataOnly && identity != SubBike) {
            SF_DEBUG_LOG("[FTMS-IN] dropped: bike-only source\n");
            return;
        }
        if (dataSubscribedMask_ && !(dataSubscribedMask_ & (1U << identity))) {
            SF_DEBUG_LOG("[FTMS-IN] dropped: unsubscribed characteristic\n");
            return;
        }
        if (dataProfileSelected_ && subscriptionProfile(identity) != profile_) {
            SF_DEBUG_LOG("[FTMS-IN] dropped: profile mismatch (selected=%s)\n", machineProfileName(profile_));
            return;
        }
        profile_ = subscriptionProfile(identity);
        dataProfileSelected_ = true;
        apply(decoded, nowMs);
    }
}

float FtmsSource::clampTo(const ftms::SupportedRange& range, float value) const {
    if (!range.valid) return value;
    if (value < range.minimum) return range.minimum;
    if (value > range.maximum) return range.maximum;
    return value;
}

bool FtmsSource::control(const ControlRequest& request, std::uint32_t) {
    if (controlState_ != ControlState::Granted || !std::isfinite(request.value)) return false;
    const std::uint32_t targets = features_.targets;
    switch (request.control) {
        case Control::Power:
            if (!(targets & ftms::kTargetPower)) return false;
            return sendControl(ftms::encodeSetTargetPower(clampTo(powerRange_, request.value)));
        case Control::Resistance: {
            if (!(targets & ftms::kTargetResistance)) return false;
            // The core works in 0..100 %; map onto the advertised device range.
            float level = request.value;
            if (resistanceRange_.valid)
                level = resistanceRange_.minimum +
                        request.value / 100.0F * (resistanceRange_.maximum - resistanceRange_.minimum);
            return sendControl(ftms::encodeSetTargetResistance(clampTo(resistanceRange_, level)));
        }
        case Control::Incline:
            if (targets & ftms::kTargetInclination)
                return sendControl(ftms::encodeSetTargetInclination(clampTo(inclinationRange_, request.value)));
            if (targets & ftms::kTargetIndoorBikeSimulation)
                return sendControl(ftms::encodeIndoorBikeSimulation(request.value));
            return false;
        case Control::Speed:
            if (!(targets & ftms::kTargetSpeed)) return false;
            return sendControl(ftms::encodeSetTargetSpeed(request.value));
        default: return false;
    }
}

TrainerCapabilities FtmsSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    if (controlState_ != ControlState::Granted) return capabilities;
    const std::uint32_t targets = features_.targets;
    capabilities.setControl(Control::Power, targets & ftms::kTargetPower);
    capabilities.setControl(Control::Resistance, targets & ftms::kTargetResistance);
    capabilities.setControl(Control::Incline, targets & (ftms::kTargetInclination | ftms::kTargetIndoorBikeSimulation));
    capabilities.setControl(Control::Speed, targets & ftms::kTargetSpeed);
    return capabilities;
}

}  // namespace protocols
}  // namespace smartfitness
