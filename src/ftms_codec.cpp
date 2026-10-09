#include "ftms/ftms_codec.hpp"

#include <cmath>

namespace smartfitness {
namespace ftms {
namespace {

void putUint16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void putSint16(Bytes& out, std::int16_t value) { putUint16(out, static_cast<std::uint16_t>(value)); }

void putUint32(Bytes& out, std::uint32_t value) {
    putUint16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    putUint16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFU));
}

long scaled(float value, float scale, long minimum, long maximum) {
    const long rounded = std::lround(static_cast<double>(value) * static_cast<double>(scale));
    return rounded < minimum ? minimum : (rounded > maximum ? maximum : rounded);
}

std::int16_t readSint16(const std::uint8_t* data) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(data[0] | (data[1] << 8)));
}

bool usable(const MetricSample& sample) { return sample.valid && std::isfinite(sample.value); }

}  // namespace

IndoorBikeData indoorBikeDataFrom(const TrainerState& state) {
    IndoorBikeData data;
    if (!state.connected) return data;
    data.speed = state.sample(Metric::Speed);
    data.cadence = state.sample(Metric::Cadence);
    data.power = state.sample(Metric::Power);
    data.heartRate = state.sample(Metric::HeartRate);
    return data;
}

Bytes encodeIndoorBikeData(const IndoorBikeData& data) {
    std::uint16_t flags = 0;
    if (!usable(data.speed)) flags |= kBikeFlagMoreData;
    if (usable(data.cadence)) flags |= kBikeFlagCadence;
    if (usable(data.power)) flags |= kBikeFlagPower;
    if (usable(data.heartRate)) flags |= kBikeFlagHeartRate;

    Bytes out;
    putUint16(out, flags);
    // Field order follows the FTMS Indoor Bike Data definition.
    if (usable(data.speed)) putUint16(out, static_cast<std::uint16_t>(scaled(data.speed.value, 100.0F, 0, 0xFFFF)));
    if (usable(data.cadence)) putUint16(out, static_cast<std::uint16_t>(scaled(data.cadence.value, 2.0F, 0, 0xFFFF)));
    if (usable(data.power)) putSint16(out, static_cast<std::int16_t>(scaled(data.power.value, 1.0F, -32768, 32767)));
    if (usable(data.heartRate)) out.push_back(static_cast<std::uint8_t>(scaled(data.heartRate.value, 1.0F, 0, 0xFF)));
    return out;
}

Bytes encodeFeature(const TrainerCapabilities& capabilities) {
    std::uint32_t features = 0;
    if (capabilities.supports(Metric::Cadence)) features |= kFeatureCadence;
    if (capabilities.supports(Metric::Incline)) features |= kFeatureInclination;
    if (capabilities.supports(Metric::HeartRate)) features |= kFeatureHeartRate;
    if (capabilities.supports(Metric::Power)) features |= kFeaturePower;

    std::uint32_t targets = 0;
    if (capabilities.supports(Control::Incline)) targets |= kTargetInclination | kTargetIndoorBikeSimulation;
    if (capabilities.supports(Control::Resistance)) targets |= kTargetResistance;
    if (capabilities.supports(Control::Power)) targets |= kTargetPower;

    Bytes out;
    putUint32(out, features);
    putUint32(out, targets);
    return out;
}

Bytes encodeSupportedInclinationRange() {
    Bytes out;  // 0.1 % resolution
    putSint16(out, static_cast<std::int16_t>(scaled(kInclineBounds.minimum, 10.0F, -32768, 32767)));
    putSint16(out, static_cast<std::int16_t>(scaled(kInclineBounds.maximum, 10.0F, -32768, 32767)));
    putUint16(out, 1);
    return out;
}

Bytes encodeSupportedResistanceRange() {
    Bytes out;  // 0.1 resolution, one unit equals one normalized percent
    putSint16(out, static_cast<std::int16_t>(scaled(kResistanceBounds.minimum, 10.0F, -32768, 32767)));
    putSint16(out, static_cast<std::int16_t>(scaled(kResistanceBounds.maximum, 10.0F, -32768, 32767)));
    putUint16(out, 10);
    return out;
}

Bytes encodeSupportedPowerRange() {
    Bytes out;  // 1 W resolution
    putSint16(out, static_cast<std::int16_t>(scaled(kPowerBounds.minimum, 1.0F, -32768, 32767)));
    putSint16(out, static_cast<std::int16_t>(scaled(kPowerBounds.maximum, 1.0F, -32768, 32767)));
    putUint16(out, 1);
    return out;
}

Bytes encodeTrainingStatus(TrainingStatus status) {
    return {0x00, static_cast<std::uint8_t>(status)};
}

Bytes encodeAdvertisingServiceData() {
    // Flags: bit 0 Fitness Machine Available. Type: bit 5 Indoor Bike Supported.
    return {0x01, 0x20, 0x00};
}

bool decodeControlPoint(const std::uint8_t* data, std::size_t length, ControlPointCommand& command) {
    command = {};
    if (!data || length == 0) return false;
    command.opcode = data[0];
    const std::uint8_t* parameters = data + 1;
    const std::size_t parameterLength = length - 1;
    const auto expect = [&command, parameterLength](std::size_t expected) {
        command.status = parameterLength == expected ? ResultCode::Success : ResultCode::InvalidParameter;
        return parameterLength == expected;
    };
    switch (static_cast<OpCode>(command.opcode)) {
        case OpCode::RequestControl:
        case OpCode::Reset:
        case OpCode::StartOrResume:
            expect(0);
            break;
        case OpCode::SetTargetInclination:
            if (!expect(2)) break;
            command.hasRequest = true;
            command.request = {Control::Incline, static_cast<float>(readSint16(parameters)) / 10.0F};
            command.statusParameters.assign(parameters, parameters + 2);
            break;
        case OpCode::SetTargetResistance:
            // FTMS 1.0 defines UINT8 (0.1); later clients may send SINT16 (0.1).
            if (parameterLength == 1) {
                command.status = ResultCode::Success;
                command.request = {Control::Resistance, static_cast<float>(parameters[0]) / 10.0F};
            } else if (parameterLength == 2) {
                command.status = ResultCode::Success;
                command.request = {Control::Resistance, static_cast<float>(readSint16(parameters)) / 10.0F};
            } else {
                command.status = ResultCode::InvalidParameter;
                break;
            }
            command.hasRequest = true;
            command.statusParameters.assign(parameters, parameters + parameterLength);
            break;
        case OpCode::SetTargetPower:
            if (!expect(2)) break;
            command.hasRequest = true;
            command.request = {Control::Power, static_cast<float>(readSint16(parameters))};
            command.statusParameters.assign(parameters, parameters + 2);
            break;
        case OpCode::StopOrPause:
            if (!expect(1)) break;
            if (parameters[0] != 0x01 && parameters[0] != 0x02) {
                command.status = ResultCode::InvalidParameter;
                break;
            }
            command.statusParameters.assign(parameters, parameters + 1);
            break;
        case OpCode::SetIndoorBikeSimulation:
            // Wind speed (SINT16, 0.001 m/s), grade (SINT16, 0.01 %), Crr (UINT8), Cw (UINT8).
            if (!expect(6)) break;
            command.hasRequest = true;
            command.request = {Control::Incline, static_cast<float>(readSint16(parameters + 2)) / 100.0F};
            command.statusParameters.assign(parameters, parameters + 6);
            break;
        default:
            command.status = ResultCode::NotSupported;
            break;
    }
    return true;
}

Bytes encodeControlPointResponse(std::uint8_t opcode, ResultCode result) {
    return {static_cast<std::uint8_t>(OpCode::ResponseCode), opcode, static_cast<std::uint8_t>(result)};
}

Bytes encodeMachineStatus(const ControlPointCommand& command) {
    StatusCode code;
    switch (static_cast<OpCode>(command.opcode)) {
        case OpCode::Reset: code = StatusCode::Reset; break;
        case OpCode::StopOrPause: code = StatusCode::StoppedOrPaused; break;
        case OpCode::StartOrResume: code = StatusCode::StartedOrResumed; break;
        case OpCode::SetTargetInclination: code = StatusCode::TargetInclineChanged; break;
        case OpCode::SetTargetResistance: code = StatusCode::TargetResistanceChanged; break;
        case OpCode::SetTargetPower: code = StatusCode::TargetPowerChanged; break;
        case OpCode::SetIndoorBikeSimulation: code = StatusCode::IndoorBikeSimulationChanged; break;
        default: return {};
    }
    Bytes out{static_cast<std::uint8_t>(code)};
    out.insert(out.end(), command.statusParameters.begin(), command.statusParameters.end());
    return out;
}

ResultCode resultCodeFor(ControlResult result) {
    switch (result) {
        case ControlResult::Applied: return ResultCode::Success;
        case ControlResult::Unsupported: return ResultCode::NotSupported;
        case ControlResult::InvalidValue: return ResultCode::InvalidParameter;
        case ControlResult::AdapterRejected: return ResultCode::OperationFailed;
        case ControlResult::Disconnected:
        case ControlResult::NotTrainer:
        case ControlResult::NoAdapter:
            return ResultCode::ControlNotPermitted;
    }
    return ResultCode::OperationFailed;
}

}  // namespace ftms
}  // namespace smartfitness
