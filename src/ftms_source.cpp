#include "protocols/ftms_source.hpp"

#include <cmath>
#include <cstdio>

namespace smartfitness {
namespace protocols {
namespace {

ble::Uuid uuid16(std::uint16_t value) { return ble::Uuid::from16(value); }
const ble::Uuid kService = ble::Uuid::from16(ftms::kServiceUuid);
const ble::Uuid kControlPoint = ble::Uuid::from16(ftms::kControlPointUuid);

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

}  // namespace

FtmsSource::FtmsSource() : FtmsSource(Options{}) {}
FtmsSource::FtmsSource(const Options& options) : options_(options) {}

ble::ConnectPlan FtmsSource::plan() const {
    ble::ConnectPlan plan;
    plan.subscribe(kService, uuid16(ftms::kIndoorBikeDataUuid));
    if (options_.bikeDataOnly) return plan;
    plan.subscribe(kService, uuid16(ftms::kTreadmillDataUuid));
    plan.subscribe(kService, uuid16(ftms::kCrossTrainerDataUuid));
    plan.subscribe(kService, uuid16(ftms::kRowerDataUuid));
    if (options_.allowControl) plan.subscribe(kService, kControlPoint, true);
    plan.read(kService, uuid16(ftms::kFeatureUuid));
    plan.read(kService, uuid16(ftms::kSupportedResistanceRangeUuid));
    plan.read(kService, uuid16(ftms::kSupportedInclinationRangeUuid));
    plan.read(kService, uuid16(ftms::kSupportedPowerRangeUuid));
    return plan;
}

void FtmsSource::reset() {
    SourceProtocol::reset();
    features_ = {};
    featuresRead_ = false;
    resistanceRange_ = {};
    inclinationRange_ = {};
    powerRange_ = {};
    controlPointSubscribed_ = false;
    controlState_ = ControlState::Unavailable;
    controlRequestedMs_ = 0;
    controlAttempts_ = 0;
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
    const std::uint32_t dataMask = (1U << SubBike) | (1U << SubTreadmill) | (1U << SubCrossTrainer) | (1U << SubRower);
    if ((subscribedMask & dataMask) == 0) {
        setError("no FTMS data characteristic");
        return false;
    }
    controlPointSubscribed_ = options_.allowControl && !options_.bikeDataOnly &&
                              (subscribedMask & (1U << SubControlPoint)) != 0;
    const std::uint32_t controllable = ftms::kTargetPower | ftms::kTargetResistance | ftms::kTargetInclination |
                                       ftms::kTargetIndoorBikeSimulation | ftms::kTargetSpeed;
    if (controlPointSubscribed_ && featuresRead_ && (features_.targets & controllable)) requestControl(nowMs);
    return true;
}

void FtmsSource::requestControl(std::uint32_t nowMs) {
    controlState_ = ControlState::Requesting;
    controlRequestedMs_ = nowMs;
    ++controlAttempts_;
    sendControl(ftms::encodeRequestControl());
}

bool FtmsSource::sendControl(const ftms::Bytes& payload) { return queueWrite(kService, kControlPoint, payload, true); }

void FtmsSource::tick(std::uint32_t nowMs) {
    if (controlState_ != ControlState::Requesting || nowMs - controlRequestedMs_ < kControlTimeoutMs) return;
    if (controlAttempts_ < kControlAttempts) {
        requestControl(nowMs);
        return;
    }
    controlState_ = ControlState::Denied;
    setError("no response to FTMS Request Control");
}

void FtmsSource::handleResponse(const std::uint8_t* data, std::size_t length) {
    ftms::ControlPointResponse response;
    if (!ftms::decodeControlPointResponse(data, length, response)) return;
    if (response.requestOpcode == static_cast<std::uint8_t>(ftms::OpCode::RequestControl)) {
        if (controlState_ != ControlState::Requesting) return;
        if (response.result == ftms::ResultCode::Success) {
            controlState_ = ControlState::Granted;
            sendControl(ftms::encodeStartOrResume());
        } else {
            controlState_ = ControlState::Denied;
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
    if (data.speed.valid) sample_.speed = data.speed;
    if (data.cadence.valid) sample_.cadence = data.cadence;
    if (data.power.valid) sample_.power = data.power;
    if (data.heartRate.valid && data.heartRate.value > 0.0F) sample_.heartRate = data.heartRate;
    if (data.incline.valid) sample_.incline = data.incline;
    if (data.resistance.valid) sample_.resistance = data.resistance;
    touch(nowMs);
}

void FtmsSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    ftms::MachineData decoded;
    bool ok = false;
    switch (index) {
        case SubBike: ok = ftms::decodeIndoorBikeData(data, length, decoded); break;
        case SubTreadmill: ok = ftms::decodeTreadmillData(data, length, decoded); break;
        case SubCrossTrainer: ok = ftms::decodeCrossTrainerData(data, length, decoded); break;
        case SubRower: ok = ftms::decodeRowerData(data, length, decoded); break;
        case SubControlPoint: handleResponse(data, length); return;
        default: return;
    }
    if (ok) apply(decoded, nowMs);
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
