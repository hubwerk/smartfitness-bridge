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
    const double raw = static_cast<double>(value) * static_cast<double>(scale);
    if (raw <= minimum) return minimum;
    if (raw >= maximum) return maximum;
    const long rounded = std::lround(raw);
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
    data = state.metrics;
    if (state.sample(Metric::Speed).valid) data.speed = state.sample(Metric::Speed);
    if (state.sample(Metric::Cadence).valid) data.cadence = state.sample(Metric::Cadence);
    if (state.sample(Metric::Power).valid) data.power = state.sample(Metric::Power);
    if (state.sample(Metric::HeartRate).valid) data.heartRate = state.sample(Metric::HeartRate);
    return data;
}

Bytes encodeIndoorBikeData(const IndoorBikeData& data) {
    return encodeMachineData(MachineProfile::IndoorBike, data);
}

TreadmillData treadmillDataFrom(const TrainerState& state) {
    TreadmillData data;
    if (!state.connected) return data;
    data = state.metrics;
    if (state.sample(Metric::Speed).valid) data.speed = state.sample(Metric::Speed);
    if (state.sample(Metric::Incline).valid) data.incline = state.sample(Metric::Incline);
    if (state.sample(Metric::HeartRate).valid) data.heartRate = state.sample(Metric::HeartRate);
    if (state.sample(Metric::Distance).valid) data.distance = state.sample(Metric::Distance);
    if (state.sample(Metric::ElapsedTime).valid) data.elapsed = state.sample(Metric::ElapsedTime);
    return data;
}

Bytes encodeTreadmillData(const TreadmillData& data) {
    return encodeMachineData(MachineProfile::Treadmill, data);
}

Bytes encodeFeature(const TrainerCapabilities& capabilities, MachineProfile profile) {
    return encodeFeature(capabilities, profile, {});
}

Bytes encodeFeature(const TrainerCapabilities& capabilities, MachineProfile profile, const FitnessMetrics& data) {
    const bool bike = profile == MachineProfile::IndoorBike;
    const bool treadmill = profile == MachineProfile::Treadmill;
    const bool cross = profile == MachineProfile::CrossTrainer;
    const bool rower = profile == MachineProfile::Rower;
    const bool climbing = profile == MachineProfile::StepClimber || profile == MachineProfile::StairClimber;
    std::uint32_t features = 0;
    if (!climbing && usable(data.averageSpeed) && !rower) features |= kFeatureAverageSpeed;
    if ((bike || rower) && (capabilities.supports(Metric::Cadence) || usable(data.cadence) || usable(data.strokeRate)))
        features |= kFeatureCadence;
    if (!climbing && (capabilities.supports(Metric::Distance) || usable(data.distance))) features |= kFeatureDistance;
    if ((bike || treadmill || cross) && (capabilities.supports(Metric::Incline) || usable(data.incline)))
        features |= kFeatureInclination;
    if (!bike && (usable(data.positiveElevationGain) || usable(data.negativeElevationGain))) features |= kFeatureElevationGain;
    if ((treadmill || rower) && (usable(data.pace) || usable(data.averagePace))) features |= kFeaturePace;
    if ((cross || climbing) && (capabilities.supports(Metric::Cadence) || usable(data.stepRate) || usable(data.stepCount)))
        features |= kFeatureStepCount;
    if ((bike || cross || rower) && usable(data.resistance)) features |= kFeatureResistanceLevel;
    if ((cross || profile == MachineProfile::StairClimber) && usable(data.strideCount)) features |= kFeatureStrideCount;
    if (usable(data.totalEnergy) && usable(data.energyPerHour) && usable(data.energyPerMinute)) features |= kFeatureExpendedEnergy;
    if (capabilities.supports(Metric::HeartRate) || usable(data.heartRate)) features |= kFeatureHeartRate;
    if (usable(data.metabolicEquivalent)) features |= kFeatureMetabolicEquivalent;
    if (capabilities.supports(Metric::ElapsedTime) || usable(data.elapsed)) features |= kFeatureElapsedTime;
    if (usable(data.remaining)) features |= kFeatureRemainingTime;
    if ((bike || rower || cross) && (capabilities.supports(Metric::Power) || usable(data.power))) features |= kFeaturePower;
    if (treadmill && usable(data.forceOnBelt) && usable(data.power)) features |= kFeaturePowerAndForce;
    std::uint32_t targets = 0;
    if ((treadmill || cross) && capabilities.supports(Control::Speed)) targets |= kTargetSpeed;
    if ((bike || treadmill || cross) && capabilities.supports(Control::Incline)) {
        targets |= kTargetInclination;
        if (bike) targets |= kTargetIndoorBikeSimulation;
    }
    if ((bike || rower || cross) && capabilities.supports(Control::Resistance)) targets |= kTargetResistance;
    if ((bike || rower || cross) && capabilities.supports(Control::Power)) targets |= kTargetPower;

    Bytes out;
    putUint32(out, features);
    putUint32(out, targets);
    return out;
}

Bytes encodeSupportedSpeedRange() {
    Bytes out;
    putUint16(out, static_cast<std::uint16_t>(scaled(kSpeedBounds.minimum, 100, 0, 65535)));
    putUint16(out, static_cast<std::uint16_t>(scaled(kSpeedBounds.maximum, 100, 0, 65535)));
    putUint16(out, 1);
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

Bytes encodeAdvertisingServiceData(MachineProfile profile) {
    std::uint8_t type = 0;
    switch (profile) {
        case MachineProfile::Treadmill: type = 1U << 0; break;
        case MachineProfile::CrossTrainer: type = 1U << 1; break;
        case MachineProfile::StepClimber: type = 1U << 2; break;
        case MachineProfile::StairClimber: type = 1U << 3; break;
        case MachineProfile::Rower: type = 1U << 4; break;
        case MachineProfile::IndoorBike: type = 1U << 5; break;
    }
    return {0x01, type, 0x00};
}

bool decodeControlPoint(const std::uint8_t* data, std::size_t length, ControlPointCommand& command, MachineProfile profile) {
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
        case OpCode::SetTargetSpeed:
            if (profile != MachineProfile::Treadmill && profile != MachineProfile::CrossTrainer) break;
            if (!expect(2)) break;
            command.request = {Control::Speed, static_cast<float>(
                static_cast<std::uint16_t>(parameters[0] | (parameters[1] << 8))) / 100.0F};
            if (command.request.value < kSpeedBounds.minimum || command.request.value > kSpeedBounds.maximum) {
                command.status = ResultCode::InvalidParameter;
                break;
            }
            command.hasRequest = true;
            command.statusParameters.assign(parameters, parameters + 2);
            break;
        case OpCode::RequestControl:
        case OpCode::Reset:
        case OpCode::StartOrResume:
            expect(0);
            break;
        case OpCode::SetTargetInclination:
            if (profile != MachineProfile::IndoorBike && profile != MachineProfile::Treadmill && profile != MachineProfile::CrossTrainer) break;
            if (!expect(2)) break;
            command.hasRequest = true;
            command.request = {Control::Incline, static_cast<float>(readSint16(parameters)) / 10.0F};
            if (profile == MachineProfile::Treadmill &&
                (command.request.value < kInclineBounds.minimum || command.request.value > kInclineBounds.maximum)) {
                command.status = ResultCode::InvalidParameter;
                command.hasRequest = false;
                break;
            }
            command.statusParameters.assign(parameters, parameters + 2);
            break;
        case OpCode::SetTargetResistance:
            if (profile != MachineProfile::IndoorBike && profile != MachineProfile::Rower && profile != MachineProfile::CrossTrainer) break;
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
            if (profile != MachineProfile::IndoorBike && profile != MachineProfile::Rower && profile != MachineProfile::CrossTrainer) break;
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
            if (profile != MachineProfile::IndoorBike) break;
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
        case OpCode::SetTargetSpeed: code = StatusCode::TargetSpeedChanged; break;
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

struct Field {
    MetricSample FitnessMetrics::* member;
    std::size_t width;
    float scale;
    bool signedValue = false;
};
struct Group {
    unsigned bit;
    std::vector<Field> fields;
};
using Layout = std::vector<Group>;
using M = FitnessMetrics;

// FTMS 1.0 sections 4.4–4.9. A flag may describe multiple indivisible
// fields; More Data (bit 0) inversely describes the first group.
const Layout& layout(MachineProfile profile) {
    static const Layout bike{
        {0, {{&M::speed, 2, 100}}}, {1, {{&M::averageSpeed, 2, 100}}},
        {2, {{&M::cadence, 2, 2}}}, {3, {{&M::averageCadence, 2, 2}}},
        {4, {{&M::distance, 3, 1}}}, {5, {{&M::resistance, 2, 1, true}}},
        {6, {{&M::power, 2, 1, true}}}, {7, {{&M::averagePower, 2, 1, true}}},
        {8, {{&M::totalEnergy, 2, 1}, {&M::energyPerHour, 2, 1}, {&M::energyPerMinute, 1, 1}}},
        {9, {{&M::heartRate, 1, 1}}}, {10, {{&M::metabolicEquivalent, 1, 10}}},
        {11, {{&M::elapsed, 2, 1}}}, {12, {{&M::remaining, 2, 1}}}
    };
    static const Layout treadmill{
        {0, {{&M::speed, 2, 100}}}, {1, {{&M::averageSpeed, 2, 100}}},
        {2, {{&M::distance, 3, 1}}},
        {3, {{&M::incline, 2, 10, true}, {&M::rampAngle, 2, 10, true}}},
        {4, {{&M::positiveElevationGain, 2, 10}, {&M::negativeElevationGain, 2, 10}}},
        {5, {{&M::pace, 1, 10}}}, {6, {{&M::averagePace, 1, 10}}},
        {7, {{&M::totalEnergy, 2, 1}, {&M::energyPerHour, 2, 1}, {&M::energyPerMinute, 1, 1}}},
        {8, {{&M::heartRate, 1, 1}}}, {9, {{&M::metabolicEquivalent, 1, 10}}},
        {10, {{&M::elapsed, 2, 1}}}, {11, {{&M::remaining, 2, 1}}},
        {12, {{&M::forceOnBelt, 2, 1, true}, {&M::power, 2, 1, true}}}
    };
    static const Layout cross{
        {0, {{&M::speed, 2, 100}}}, {1, {{&M::averageSpeed, 2, 100}}},
        {2, {{&M::distance, 3, 1}}},
        {3, {{&M::stepRate, 2, 1}, {&M::averageStepRate, 2, 1}}},
        {4, {{&M::strideCount, 2, 10}}},
        {5, {{&M::positiveElevationGain, 2, 1}, {&M::negativeElevationGain, 2, 1}}},
        {6, {{&M::incline, 2, 10, true}, {&M::rampAngle, 2, 10, true}}},
        {7, {{&M::resistance, 2, 10, true}}},
        {8, {{&M::power, 2, 1, true}}}, {9, {{&M::averagePower, 2, 1, true}}},
        {10, {{&M::totalEnergy, 2, 1}, {&M::energyPerHour, 2, 1}, {&M::energyPerMinute, 1, 1}}},
        {11, {{&M::heartRate, 1, 1}}}, {12, {{&M::metabolicEquivalent, 1, 10}}},
        {13, {{&M::elapsed, 2, 1}}}, {14, {{&M::remaining, 2, 1}}}
    };
    static const Layout rower{
        {0, {{&M::strokeRate, 1, 2}, {&M::strokeCount, 2, 1}}},
        {1, {{&M::averageStrokeRate, 1, 2}}}, {2, {{&M::distance, 3, 1}}},
        {3, {{&M::pace, 2, 1}}}, {4, {{&M::averagePace, 2, 1}}},
        {5, {{&M::power, 2, 1, true}}}, {6, {{&M::averagePower, 2, 1, true}}},
        {7, {{&M::resistance, 2, 1, true}}},
        {8, {{&M::totalEnergy, 2, 1}, {&M::energyPerHour, 2, 1}, {&M::energyPerMinute, 1, 1}}},
        {9, {{&M::heartRate, 1, 1}}}, {10, {{&M::metabolicEquivalent, 1, 10}}},
        {11, {{&M::elapsed, 2, 1}}}, {12, {{&M::remaining, 2, 1}}}
    };
    static const Layout step{
        {0, {{&M::floors, 2, 1}, {&M::stepCount, 2, 1}}},
        {1, {{&M::stepRate, 2, 1}}}, {2, {{&M::averageStepRate, 2, 1}}},
        {3, {{&M::positiveElevationGain, 2, 1}}},
        {4, {{&M::totalEnergy, 2, 1}, {&M::energyPerHour, 2, 1}, {&M::energyPerMinute, 1, 1}}},
        {5, {{&M::heartRate, 1, 1}}}, {6, {{&M::metabolicEquivalent, 1, 10}}},
        {7, {{&M::elapsed, 2, 1}}}, {8, {{&M::remaining, 2, 1}}}
    };
    static const Layout stair{
        {0, {{&M::floors, 2, 1}}}, {1, {{&M::stepRate, 2, 1}}},
        {2, {{&M::averageStepRate, 2, 1}}}, {3, {{&M::positiveElevationGain, 2, 1}}},
        {4, {{&M::strideCount, 2, 1}}},
        {5, {{&M::totalEnergy, 2, 1}, {&M::energyPerHour, 2, 1}, {&M::energyPerMinute, 1, 1}}},
        {6, {{&M::heartRate, 1, 1}}}, {7, {{&M::metabolicEquivalent, 1, 10}}},
        {8, {{&M::elapsed, 2, 1}}}, {9, {{&M::remaining, 2, 1}}}
    };
    static const Layout none;
    switch (profile) {
        case MachineProfile::IndoorBike: return bike;
        case MachineProfile::Treadmill: return treadmill;
        case MachineProfile::CrossTrainer: return cross;
        case MachineProfile::Rower: return rower;
        case MachineProfile::StepClimber: return step;
        case MachineProfile::StairClimber: return stair;
    }
    return none;
}

std::size_t flagWidth(MachineProfile profile) {
    return profile == MachineProfile::CrossTrainer ? 3 : 2;
}
void putFlags(Bytes& packet, std::uint32_t flags) {
    for (std::size_t i = 0; i < packet.size(); ++i)
        packet[i] = static_cast<std::uint8_t>(flags >> (8 * i));
}
bool present(const Group& group, const FitnessMetrics& data) {
    for (const auto& field : group.fields)
        if (!usable(data.*(field.member))) return false;
    return true;
}
Bytes encodeGroup(const Group& group, const FitnessMetrics& data) {
    Bytes out;
    for (const auto& field : group.fields) {
        const long minimum = field.signedValue ? -32768 : 0;
        const long maximum = field.signedValue ? 32767 : (1L << (8 * field.width)) - 1;
        const auto raw = static_cast<std::uint32_t>(scaled((data.*(field.member)).value, field.scale, minimum, maximum));
        for (std::size_t i = 0; i < field.width; ++i)
            out.push_back(static_cast<std::uint8_t>(raw >> (8 * i)));
    }
    return out;
}
std::uint32_t directionFlags(MachineProfile profile, const FitnessMetrics& data) {
    return profile == MachineProfile::CrossTrainer && usable(data.movementDirection) &&
        data.movementDirection.value != 0 ? 1U << 15 : 0;
}

}  // namespace

bool decodeMachineData(MachineProfile profile, const std::uint8_t* data, std::size_t length, MachineData& out) {
    out = {};
    const auto& fields = layout(profile);
    if (fields.empty()) return false;
    FieldReader reader(data, length);
    const auto flags = reader.uint(flagWidth(profile));
    MachineData decoded;
    for (const auto& group : fields) {
        const bool included = group.bit == 0 ? !(flags & 1U) : (flags & (1U << group.bit)) != 0;
        if (!included) continue;
        for (const auto& field : group.fields) {
            const float raw = field.signedValue ? static_cast<float>(reader.sint16()) :
                static_cast<float>(reader.uint(field.width));
            decoded.*(field.member) = sample(raw / field.scale);
        }
    }
    if (!reader.ok()) return false;
    if (profile == MachineProfile::CrossTrainer)
        decoded.movementDirection = sample((flags & (1U << 15)) ? 1.0F : 0.0F);
    out = mapMachineData(decoded, profile, profile);
    return true;
}

Bytes encodeMachineData(MachineProfile profile, const FitnessMetrics& data) {
    if (layout(profile).empty()) return {};
    Bytes out(flagWidth(profile), 0);
    std::uint32_t flags = 1U | directionFlags(profile, data);
    for (const auto& group : layout(profile)) {
        if (!present(group, data)) continue;
        flags = group.bit == 0 ? flags & ~1U : flags | (1U << group.bit);
        const Bytes encoded = encodeGroup(group, data);
        out.insert(out.end(), encoded.begin(), encoded.end());
    }
    Bytes header(flagWidth(profile));
    putFlags(header, flags);
    for (std::size_t i = 0; i < header.size(); ++i) out[i] = header[i];
    return out;
}

std::vector<Bytes> splitMachineData(MachineProfile profile, const FitnessMetrics& data, std::size_t maxPayload) {
    const auto width = flagWidth(profile);
    if (layout(profile).empty() || maxPayload < width) return {};
    std::vector<Bytes> packets;
    Bytes packet(width, 0);
    std::uint32_t flags = 1U | directionFlags(profile, data);
    const auto flush = [&]() {
        Bytes header(width);
        putFlags(header, flags);
        for (std::size_t i = 0; i < width; ++i) packet[i] = header[i];
        packets.push_back(packet);
        packet.assign(width, 0);
        flags = 1U | directionFlags(profile, data);
    };
    // The mandatory group is emitted last: preceding fragments set More Data.
    // Within every fragment optional groups retain their specification order.
    for (const auto& group : layout(profile)) {
        if (group.bit == 0 || !present(group, data)) continue;
        const auto encoded = encodeGroup(group, data);
        if (encoded.size() > maxPayload - width) return {};
        if (encoded.size() > maxPayload - packet.size()) flush();
        flags |= 1U << group.bit;
        packet.insert(packet.end(), encoded.begin(), encoded.end());
    }
    const auto& mandatory = layout(profile).front();
    if (present(mandatory, data)) {
        const auto encoded = encodeGroup(mandatory, data);
        if (encoded.size() > maxPayload - width) return {};
        if (encoded.size() > maxPayload - packet.size()) flush();
        flags &= ~1U;
        // Mandatory fields precede optional fields, even in the final fragment.
        packet.insert(packet.begin() + static_cast<Bytes::difference_type>(width), encoded.begin(), encoded.end());
    }
    if (packet.size() > width || packets.empty()) flush();
    return packets;
}

FitnessMetrics mapMachineData(const FitnessMetrics& data, MachineProfile input, MachineProfile output) {
    FitnessMetrics mapped = data;
    const auto reciprocal = [](MetricSample& destination, const MetricSample& divisor) {
        if (usable(destination) || !usable(divisor) || divisor.value <= 0) return;
        const float value = 1800.0F / divisor.value;
        if (std::isfinite(value)) destination = sample(value);
    };
    if (input == MachineProfile::Rower) {
        reciprocal(mapped.speed, mapped.pace);
        if (!usable(mapped.cadence) && usable(mapped.strokeRate)) mapped.cadence = mapped.strokeRate;
    }
    if (input == MachineProfile::CrossTrainer || input == MachineProfile::StepClimber || input == MachineProfile::StairClimber) {
        if (!usable(mapped.cadence) && usable(mapped.stepRate)) mapped.cadence = mapped.stepRate;
    }
    // Pace units differ between rowers and treadmills; never copy them across.
    if (input != output && (input == MachineProfile::Rower || output == MachineProfile::Rower)) {
        mapped.pace = {};
        mapped.averagePace = {};
    }
    if (output == MachineProfile::Rower) {
        reciprocal(mapped.pace, mapped.speed);
        if (!usable(mapped.strokeRate) && usable(mapped.cadence)) mapped.strokeRate = mapped.cadence;
    }
    if (output == MachineProfile::CrossTrainer || output == MachineProfile::StepClimber || output == MachineProfile::StairClimber) {
        if (!usable(mapped.stepRate) && usable(mapped.cadence)) mapped.stepRate = mapped.cadence;
    }
    return mapped;
}

bool decodeIndoorBikeData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeMachineData(MachineProfile::IndoorBike, data, length, out);
}
bool decodeTreadmillData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeMachineData(MachineProfile::Treadmill, data, length, out);
}
bool decodeCrossTrainerData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeMachineData(MachineProfile::CrossTrainer, data, length, out);
}
bool decodeRowerData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeMachineData(MachineProfile::Rower, data, length, out);
}
bool decodeStepClimberData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeMachineData(MachineProfile::StepClimber, data, length, out);
}
bool decodeStairClimberData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeMachineData(MachineProfile::StairClimber, data, length, out);
}
Bytes encodeCrossTrainerData(const FitnessMetrics& data) { return encodeMachineData(MachineProfile::CrossTrainer, data); }
Bytes encodeRowerData(const FitnessMetrics& data) { return encodeMachineData(MachineProfile::Rower, data); }
Bytes encodeStepClimberData(const FitnessMetrics& data) { return encodeMachineData(MachineProfile::StepClimber, data); }
Bytes encodeStairClimberData(const FitnessMetrics& data) { return encodeMachineData(MachineProfile::StairClimber, data); }

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
