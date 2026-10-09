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

namespace {

// Bounds-checked little-endian reader for client-side decoding.
class FieldReader {
public:
    FieldReader(const std::uint8_t* data, std::size_t length) : data_(data), length_(data ? length : 0) {}
    bool ok() const { return ok_; }
    bool skip(std::size_t count) {
        if (!ok_ || length_ - offset_ < count) return ok_ = false;
        offset_ += count;
        return true;
    }
    std::uint32_t uint(std::size_t width) {
        if (!ok_ || length_ - offset_ < width) {
            ok_ = false;
            return 0;
        }
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < width; ++index)
            value |= static_cast<std::uint32_t>(data_[offset_ + index]) << (8 * index);
        offset_ += width;
        return value;
    }
    std::int32_t sint16() { return static_cast<std::int16_t>(static_cast<std::uint16_t>(uint(2))); }

private:
    const std::uint8_t* data_;
    std::size_t length_;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

MetricSample sample(float value) { return {value, true}; }

}  // namespace

bool decodeIndoorBikeData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) out.speed = sample(static_cast<float>(reader.uint(2)) / 100.0F);
    if (flags & (1U << 1)) reader.skip(2);  // average speed
    if (flags & (1U << 2)) out.cadence = sample(static_cast<float>(reader.uint(2)) / 2.0F);
    if (flags & (1U << 3)) reader.skip(2);  // average cadence
    if (flags & (1U << 4)) out.distance = sample(static_cast<float>(reader.uint(3)));
    if (flags & (1U << 5)) out.resistance = sample(static_cast<float>(reader.sint16()));
    if (flags & (1U << 6)) out.power = sample(static_cast<float>(reader.sint16()));
    if (flags & (1U << 7)) reader.skip(2);  // average power
    if (flags & (1U << 8)) reader.skip(5);  // total energy, per hour, per minute
    if (flags & (1U << 9)) out.heartRate = sample(static_cast<float>(reader.uint(1)));
    if (!reader.ok()) out = {};
    return reader.ok();
}

bool decodeTreadmillData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) out.speed = sample(static_cast<float>(reader.uint(2)) / 100.0F);
    if (flags & (1U << 1)) reader.skip(2);  // average speed
    if (flags & (1U << 2)) out.distance = sample(static_cast<float>(reader.uint(3)));
    if (flags & (1U << 3)) {
        out.incline = sample(static_cast<float>(reader.sint16()) / 10.0F);
        reader.skip(2);  // ramp angle
    }
    if (flags & (1U << 4)) reader.skip(4);  // positive and negative elevation gain
    if (flags & (1U << 5)) reader.skip(1);  // instantaneous pace
    if (flags & (1U << 6)) reader.skip(1);  // average pace
    if (flags & (1U << 7)) reader.skip(5);  // expended energy
    if (flags & (1U << 8)) out.heartRate = sample(static_cast<float>(reader.uint(1)));
    if (flags & (1U << 9)) reader.skip(1);   // metabolic equivalent
    if (flags & (1U << 10)) reader.skip(2);  // elapsed time
    if (flags & (1U << 11)) reader.skip(2);  // remaining time
    if (flags & (1U << 12)) {
        reader.skip(2);  // force on belt
        out.power = sample(static_cast<float>(reader.sint16()));
    }
    if (!reader.ok()) out = {};
    return reader.ok();
}

bool decodeCrossTrainerData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(3);
    if (!(flags & (1U << 0))) out.speed = sample(static_cast<float>(reader.uint(2)) / 100.0F);
    if (flags & (1U << 1)) reader.skip(2);  // average speed
    if (flags & (1U << 2)) out.distance = sample(static_cast<float>(reader.uint(3)));
    if (flags & (1U << 3)) {
        out.cadence = sample(static_cast<float>(reader.uint(2)));  // steps per minute
        reader.skip(2);                                            // average step rate
    }
    if (flags & (1U << 4)) reader.skip(2);  // stride count
    if (flags & (1U << 5)) reader.skip(4);  // elevation gain
    if (flags & (1U << 6)) {
        out.incline = sample(static_cast<float>(reader.sint16()) / 10.0F);
        reader.skip(2);  // ramp angle
    }
    if (flags & (1U << 7)) out.resistance = sample(static_cast<float>(reader.sint16()) / 10.0F);
    if (flags & (1U << 8)) out.power = sample(static_cast<float>(reader.sint16()));
    if (flags & (1U << 9)) reader.skip(2);   // average power
    if (flags & (1U << 10)) reader.skip(5);  // expended energy
    if (flags & (1U << 11)) out.heartRate = sample(static_cast<float>(reader.uint(1)));
    if (!reader.ok()) out = {};
    return reader.ok();
}

bool decodeRowerData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) {
        out.cadence = sample(static_cast<float>(reader.uint(1)) / 2.0F);  // strokes per minute
        reader.skip(2);                                                  // stroke count
    }
    if (flags & (1U << 1)) reader.skip(1);  // average stroke rate
    if (flags & (1U << 2)) out.distance = sample(static_cast<float>(reader.uint(3)));
    if (flags & (1U << 3)) {
        // Pace in seconds per 500 m; 0 means not moving.
        const std::uint32_t pace = reader.uint(2);
        out.speed = sample(pace == 0 ? 0.0F : 1800.0F / static_cast<float>(pace));
    }
    if (flags & (1U << 4)) reader.skip(2);  // average pace
    if (flags & (1U << 5)) out.power = sample(static_cast<float>(reader.sint16()));
    if (flags & (1U << 6)) reader.skip(2);  // average power
    if (flags & (1U << 7)) out.resistance = sample(static_cast<float>(reader.sint16()));
    if (flags & (1U << 8)) reader.skip(5);  // expended energy
    if (flags & (1U << 9)) out.heartRate = sample(static_cast<float>(reader.uint(1)));
    if (!reader.ok()) out = {};
    return reader.ok();
}

bool decodeFeature(const std::uint8_t* data, std::size_t length, Features& out) {
    if (!data || length < 8) return false;
    FieldReader reader(data, length);
    out.machine = reader.uint(4);
    out.targets = reader.uint(4);
    return true;
}

bool decodeSupportedRange(const std::uint8_t* data, std::size_t length, float resolution, SupportedRange& out) {
    out = {};
    if (!data || length < 6) return false;
    FieldReader reader(data, length);
    out.minimum = static_cast<float>(reader.sint16()) * resolution;
    out.maximum = static_cast<float>(reader.sint16()) * resolution;
    out.increment = static_cast<float>(reader.uint(2)) * resolution;
    out.valid = out.maximum >= out.minimum;
    return out.valid;
}

bool decodeControlPointResponse(const std::uint8_t* data, std::size_t length, ControlPointResponse& out) {
    if (!data || length < 3 || data[0] != static_cast<std::uint8_t>(OpCode::ResponseCode)) return false;
    out.requestOpcode = data[1];
    out.result = static_cast<ResultCode>(data[2]);
    return true;
}

Bytes encodeRequestControl() { return {static_cast<std::uint8_t>(OpCode::RequestControl)}; }
Bytes encodeStartOrResume() { return {static_cast<std::uint8_t>(OpCode::StartOrResume)}; }

Bytes encodeSetTargetPower(float watts) {
    Bytes out{static_cast<std::uint8_t>(OpCode::SetTargetPower)};
    putSint16(out, static_cast<std::int16_t>(scaled(watts, 1.0F, -32768, 32767)));
    return out;
}

Bytes encodeSetTargetResistance(float level) {
    Bytes out{static_cast<std::uint8_t>(OpCode::SetTargetResistance)};
    putSint16(out, static_cast<std::int16_t>(scaled(level, 10.0F, -32768, 32767)));
    return out;
}

Bytes encodeSetTargetInclination(float percent) {
    Bytes out{static_cast<std::uint8_t>(OpCode::SetTargetInclination)};
    putSint16(out, static_cast<std::int16_t>(scaled(percent, 10.0F, -32768, 32767)));
    return out;
}

Bytes encodeSetTargetSpeed(float kmh) {
    Bytes out{0x02};  // Set Target Speed, UINT16 0.01 km/h
    putUint16(out, static_cast<std::uint16_t>(scaled(kmh, 100.0F, 0, 0xFFFF)));
    return out;
}

Bytes encodeIndoorBikeSimulation(float gradePercent, float windSpeed, float crr, float cw) {
    Bytes out{static_cast<std::uint8_t>(OpCode::SetIndoorBikeSimulation)};
    putSint16(out, static_cast<std::int16_t>(scaled(windSpeed, 1000.0F, -32768, 32767)));
    putSint16(out, static_cast<std::int16_t>(scaled(gradePercent, 100.0F, -32768, 32767)));
    out.push_back(static_cast<std::uint8_t>(scaled(crr, 10000.0F, 0, 0xFF)));
    out.push_back(static_cast<std::uint8_t>(scaled(cw, 100.0F, 0, 0xFF)));
    return out;
}

}  // namespace ftms
}  // namespace smartfitness
