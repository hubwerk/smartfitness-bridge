#include "ftms/ftms_codec.hpp"

#include <cmath>
#include <initializer_list>

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

// ---------------------------------------------------------------------------
// Machine data encoding
// ---------------------------------------------------------------------------

namespace {

void putUint24(Bytes& out, std::uint32_t value) {
    putUint16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
}

// Writes an optional field in its wire type; a missing grouped companion is 0.
void put(Bytes& out, const std::optional<std::uint8_t>& value) { out.push_back(value.value_or(0)); }
void put(Bytes& out, const std::optional<std::uint16_t>& value) { putUint16(out, value.value_or(0)); }
void put(Bytes& out, const std::optional<std::int16_t>& value) { putSint16(out, value.value_or(0)); }
void put24(Bytes& out, const std::optional<std::uint32_t>& value) { putUint24(out, value.value_or(0)); }

template <typename... T>
bool any(const T&... values) { return (values.has_value() || ...); }

template <typename Data>
bool hasEnergy(const Data& data) { return any(data.totalEnergy, data.energyPerHour, data.energyPerMinute); }

template <typename Data>
void putEnergy(Bytes& out, const Data& data) {
    putUint16(out, data.totalEnergy.value_or(kEnergyNotAvailable16));
    putUint16(out, data.energyPerHour.value_or(kEnergyNotAvailable16));
    out.push_back(data.energyPerMinute.value_or(kEnergyNotAvailable8));
}

// Collects the fields of one data record. Each flagged field group is kept
// as a segment so an oversized record can be split into several
// notifications ("More Data", see splitRecords()).
class FlagWriter {
public:
    explicit FlagWriter(std::size_t width) : width_(width) {}
    Bytes& out() { return out_; }
    // Bit 0 is the inverted "More Data" flag: set when the first field group
    // is absent. Other bits announce the field group written next.
    void set(unsigned bit) {
        if (bit == 0) moreData_ = true;
        else segments_.push_back({bit, out_.size()});
    }
    // Status flag without a field (e.g. Cross Trainer movement direction).
    void status(unsigned bit) { status_ |= 1UL << bit; }

    Bytes finish() const {
        unsigned long flags = status_ | (moreData_ ? 1UL : 0UL);
        for (const auto& segment : segments_) flags |= 1UL << segment.bit;
        return record(flags, out_.begin(), out_.end());
    }

    // Splits into records of at most maxLength bytes. Every record but the
    // last sets "More Data"; the first field group closes the last record.
    std::vector<Bytes> split(std::size_t maxLength) const {
        const std::size_t capacity = maxLength > width_ ? maxLength - width_ : 1;
        const std::size_t headEnd = segments_.empty() ? out_.size() : segments_.front().offset;
        std::vector<Bytes> records;
        unsigned long flags = 0;
        Bytes data;
        const auto flush = [&] {
            records.push_back(record(status_ | 1UL | flags, data.begin(), data.end()));
            flags = 0;
            data.clear();
        };
        for (std::size_t index = 0; index < segments_.size(); ++index) {
            const std::size_t begin = segments_[index].offset;
            const std::size_t end = index + 1 < segments_.size() ? segments_[index + 1].offset : out_.size();
            if (!data.empty() && data.size() + (end - begin) > capacity) flush();
            flags |= 1UL << segments_[index].bit;
            data.insert(data.end(), out_.begin() + static_cast<std::ptrdiff_t>(begin),
                        out_.begin() + static_cast<std::ptrdiff_t>(end));
        }
        if (!data.empty() && data.size() + headEnd > capacity) flush();
        Bytes last(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(headEnd));
        last.insert(last.end(), data.begin(), data.end());
        records.push_back(record(status_ | flags | (moreData_ ? 1UL : 0UL), last.begin(), last.end()));
        return records;
    }

private:
    struct Segment {
        unsigned bit;
        std::size_t offset;
    };

    Bytes record(unsigned long flags, Bytes::const_iterator begin, Bytes::const_iterator end) const {
        Bytes result;
        for (std::size_t index = 0; index < width_; ++index)
            result.push_back(static_cast<std::uint8_t>((flags >> (8 * index)) & 0xFFU));
        result.insert(result.end(), begin, end);
        return result;
    }

    std::size_t width_;
    bool moreData_ = false;
    unsigned long status_ = 0;
    std::vector<Segment> segments_;
    Bytes out_;
};

FlagWriter writeRecord(const IndoorBikeData& data) {
    FlagWriter writer(2);
    Bytes& out = writer.out();
    if (data.instantaneousSpeed) put(out, data.instantaneousSpeed);
    else writer.set(0);  // More Data: no instantaneous speed in this record
    if (data.averageSpeed) writer.set(1), put(out, data.averageSpeed);
    if (data.instantaneousCadence) writer.set(2), put(out, data.instantaneousCadence);
    if (data.averageCadence) writer.set(3), put(out, data.averageCadence);
    if (data.totalDistance) writer.set(4), put24(out, data.totalDistance);
    if (data.resistanceLevel) writer.set(5), put(out, data.resistanceLevel);
    if (data.instantaneousPower) writer.set(6), put(out, data.instantaneousPower);
    if (data.averagePower) writer.set(7), put(out, data.averagePower);
    if (hasEnergy(data)) writer.set(8), putEnergy(out, data);
    if (data.heartRate) writer.set(9), put(out, data.heartRate);
    if (data.metabolicEquivalent) writer.set(10), put(out, data.metabolicEquivalent);
    if (data.elapsedTime) writer.set(11), put(out, data.elapsedTime);
    if (data.remainingTime) writer.set(12), put(out, data.remainingTime);
    return writer;
}

FlagWriter writeRecord(const TreadmillData& data) {
    FlagWriter writer(2);
    Bytes& out = writer.out();
    if (data.instantaneousSpeed) put(out, data.instantaneousSpeed);
    else writer.set(0);
    if (data.averageSpeed) writer.set(1), put(out, data.averageSpeed);
    if (data.totalDistance) writer.set(2), put24(out, data.totalDistance);
    if (any(data.inclination, data.rampAngleSetting))
        writer.set(3), put(out, data.inclination), put(out, data.rampAngleSetting);
    if (any(data.positiveElevationGain, data.negativeElevationGain))
        writer.set(4), put(out, data.positiveElevationGain), put(out, data.negativeElevationGain);
    if (data.instantaneousPace) writer.set(5), put(out, data.instantaneousPace);
    if (data.averagePace) writer.set(6), put(out, data.averagePace);
    if (hasEnergy(data)) writer.set(7), putEnergy(out, data);
    if (data.heartRate) writer.set(8), put(out, data.heartRate);
    if (data.metabolicEquivalent) writer.set(9), put(out, data.metabolicEquivalent);
    if (data.elapsedTime) writer.set(10), put(out, data.elapsedTime);
    if (data.remainingTime) writer.set(11), put(out, data.remainingTime);
    if (any(data.forceOnBelt, data.powerOutput))
        writer.set(12), put(out, data.forceOnBelt), put(out, data.powerOutput);
    return writer;
}

FlagWriter writeRecord(const CrossTrainerData& data) {
    FlagWriter writer(3);
    Bytes& out = writer.out();
    if (data.instantaneousSpeed) put(out, data.instantaneousSpeed);
    else writer.set(0);
    if (data.averageSpeed) writer.set(1), put(out, data.averageSpeed);
    if (data.totalDistance) writer.set(2), put24(out, data.totalDistance);
    if (any(data.stepPerMinute, data.averageStepRate))
        writer.set(3), put(out, data.stepPerMinute), put(out, data.averageStepRate);
    if (data.strideCount) writer.set(4), put(out, data.strideCount);
    if (any(data.positiveElevationGain, data.negativeElevationGain))
        writer.set(5), put(out, data.positiveElevationGain), put(out, data.negativeElevationGain);
    if (any(data.inclination, data.rampAngleSetting))
        writer.set(6), put(out, data.inclination), put(out, data.rampAngleSetting);
    if (data.resistanceLevel) writer.set(7), put(out, data.resistanceLevel);
    if (data.instantaneousPower) writer.set(8), put(out, data.instantaneousPower);
    if (data.averagePower) writer.set(9), put(out, data.averagePower);
    if (hasEnergy(data)) writer.set(10), putEnergy(out, data);
    if (data.heartRate) writer.set(11), put(out, data.heartRate);
    if (data.metabolicEquivalent) writer.set(12), put(out, data.metabolicEquivalent);
    if (data.elapsedTime) writer.set(13), put(out, data.elapsedTime);
    if (data.remainingTime) writer.set(14), put(out, data.remainingTime);
    if (data.movementBackward) writer.status(15);
    return writer;
}

FlagWriter writeRecord(const StepClimberData& data) {
    FlagWriter writer(2);
    Bytes& out = writer.out();
    if (any(data.floors, data.stepCount)) put(out, data.floors), put(out, data.stepCount);
    else writer.set(0);
    if (data.stepPerMinute) writer.set(1), put(out, data.stepPerMinute);
    if (data.averageStepRate) writer.set(2), put(out, data.averageStepRate);
    if (data.positiveElevationGain) writer.set(3), put(out, data.positiveElevationGain);
    if (hasEnergy(data)) writer.set(4), putEnergy(out, data);
    if (data.heartRate) writer.set(5), put(out, data.heartRate);
    if (data.metabolicEquivalent) writer.set(6), put(out, data.metabolicEquivalent);
    if (data.elapsedTime) writer.set(7), put(out, data.elapsedTime);
    if (data.remainingTime) writer.set(8), put(out, data.remainingTime);
    return writer;
}

FlagWriter writeRecord(const StairClimberData& data) {
    FlagWriter writer(2);
    Bytes& out = writer.out();
    if (data.floors) put(out, data.floors);
    else writer.set(0);
    if (data.stepPerMinute) writer.set(1), put(out, data.stepPerMinute);
    if (data.averageStepRate) writer.set(2), put(out, data.averageStepRate);
    if (data.positiveElevationGain) writer.set(3), put(out, data.positiveElevationGain);
    if (data.strideCount) writer.set(4), put(out, data.strideCount);
    if (hasEnergy(data)) writer.set(5), putEnergy(out, data);
    if (data.heartRate) writer.set(6), put(out, data.heartRate);
    if (data.metabolicEquivalent) writer.set(7), put(out, data.metabolicEquivalent);
    if (data.elapsedTime) writer.set(8), put(out, data.elapsedTime);
    if (data.remainingTime) writer.set(9), put(out, data.remainingTime);
    return writer;
}

FlagWriter writeRecord(const RowerData& data) {
    FlagWriter writer(2);
    Bytes& out = writer.out();
    if (any(data.strokeRate, data.strokeCount)) put(out, data.strokeRate), put(out, data.strokeCount);
    else writer.set(0);
    if (data.averageStrokeRate) writer.set(1), put(out, data.averageStrokeRate);
    if (data.totalDistance) writer.set(2), put24(out, data.totalDistance);
    if (data.instantaneousPace) writer.set(3), put(out, data.instantaneousPace);
    if (data.averagePace) writer.set(4), put(out, data.averagePace);
    if (data.instantaneousPower) writer.set(5), put(out, data.instantaneousPower);
    if (data.averagePower) writer.set(6), put(out, data.averagePower);
    if (data.resistanceLevel) writer.set(7), put(out, data.resistanceLevel);
    if (hasEnergy(data)) writer.set(8), putEnergy(out, data);
    if (data.heartRate) writer.set(9), put(out, data.heartRate);
    if (data.metabolicEquivalent) writer.set(10), put(out, data.metabolicEquivalent);
    if (data.elapsedTime) writer.set(11), put(out, data.elapsedTime);
    if (data.remainingTime) writer.set(12), put(out, data.remainingTime);
    return writer;
}

}  // namespace

Bytes encodeIndoorBikeData(const IndoorBikeData& data) { return writeRecord(data).finish(); }
Bytes encodeTreadmillData(const TreadmillData& data) { return writeRecord(data).finish(); }
Bytes encodeCrossTrainerData(const CrossTrainerData& data) { return writeRecord(data).finish(); }
Bytes encodeStepClimberData(const StepClimberData& data) { return writeRecord(data).finish(); }
Bytes encodeStairClimberData(const StairClimberData& data) { return writeRecord(data).finish(); }
Bytes encodeRowerData(const RowerData& data) { return writeRecord(data).finish(); }

std::vector<Bytes> encodeRecords(const IndoorBikeData& data, std::size_t maxLength) { return writeRecord(data).split(maxLength); }
std::vector<Bytes> encodeRecords(const TreadmillData& data, std::size_t maxLength) { return writeRecord(data).split(maxLength); }
std::vector<Bytes> encodeRecords(const CrossTrainerData& data, std::size_t maxLength) { return writeRecord(data).split(maxLength); }
std::vector<Bytes> encodeRecords(const StepClimberData& data, std::size_t maxLength) { return writeRecord(data).split(maxLength); }
std::vector<Bytes> encodeRecords(const StairClimberData& data, std::size_t maxLength) { return writeRecord(data).split(maxLength); }
std::vector<Bytes> encodeRecords(const RowerData& data, std::size_t maxLength) { return writeRecord(data).split(maxLength); }

// ---------------------------------------------------------------------------
// Output mapping: core metrics -> profile fields
// ---------------------------------------------------------------------------

namespace {

constexpr float kPi = 3.14159265358979323846F;

class StateReader {
public:
    explicit StateReader(const TrainerState& state) : state_(state) {}

    std::optional<float> value(Metric metric) const {
        if (!state_.connected) return std::nullopt;
        const auto& sample = state_.sample(metric);
        if (!usable(sample)) return std::nullopt;
        return sample.value;
    }
    std::optional<std::uint8_t> u8(Metric metric, float scale) const { return u8(value(metric), scale); }
    std::optional<std::uint16_t> u16(Metric metric, float scale) const { return u16(value(metric), scale); }
    std::optional<std::uint32_t> u24(Metric metric, float scale) const {
        const auto v = value(metric);
        if (!v) return std::nullopt;
        return static_cast<std::uint32_t>(scaled(*v, scale, 0, 0xFFFFFF));
    }
    std::optional<std::int16_t> s16(Metric metric, float scale) const { return s16(value(metric), scale); }

    static std::optional<std::uint8_t> u8(std::optional<float> v, float scale) {
        if (!v) return std::nullopt;
        return static_cast<std::uint8_t>(scaled(*v, scale, 0, 0xFF));
    }
    static std::optional<std::uint16_t> u16(std::optional<float> v, float scale) {
        if (!v) return std::nullopt;
        return static_cast<std::uint16_t>(scaled(*v, scale, 0, 0xFFFF));
    }
    static std::optional<std::int16_t> s16(std::optional<float> v, float scale) {
        if (!v) return std::nullopt;
        return static_cast<std::int16_t>(scaled(*v, scale, -32768, 32767));
    }

    // Energy sub-fields: the "not available" values are reserved.
    template <typename Data>
    void energy(Data& data) const {
        data.totalEnergy = u16(Metric::TotalEnergy, 1.0F);
        data.energyPerHour = u16(Metric::EnergyPerHour, 1.0F);
        data.energyPerMinute = u8(Metric::EnergyPerMinute, 1.0F);
        if (data.totalEnergy == kEnergyNotAvailable16) data.totalEnergy = kEnergyNotAvailable16 - 1;
        if (data.energyPerHour == kEnergyNotAvailable16) data.energyPerHour = kEnergyNotAvailable16 - 1;
        if (data.energyPerMinute == kEnergyNotAvailable8) data.energyPerMinute = kEnergyNotAvailable8 - 1;
    }
    template <typename Data>
    void common(Data& data) const {
        energy(data);
        data.heartRate = u8(Metric::HeartRate, 1.0F);
        data.metabolicEquivalent = u8(Metric::MetabolicEquivalent, 10.0F);
        data.elapsedTime = u16(Metric::ElapsedTime, 1.0F);
        data.remainingTime = u16(Metric::RemainingTime, 1.0F);
    }
    // Ramp angle from the reported value, else derived from the grade.
    std::optional<float> rampAngle() const {
        if (const auto angle = value(Metric::RampAngle)) return angle;
        if (const auto grade = value(Metric::Incline)) return std::atan(*grade / 100.0F) * 180.0F / kPi;
        return std::nullopt;
    }
    // Grade in % from the reported value, else derived from the ramp angle
    // (FTMS carries inclination and ramp angle only together).
    std::optional<float> incline() const {
        if (const auto grade = value(Metric::Incline)) return grade;
        if (const auto angle = value(Metric::RampAngle)) return std::tan(*angle * kPi / 180.0F) * 100.0F;
        return std::nullopt;
    }
    // Pace in s/500 m from the reported value, else derived from a speed.
    std::optional<float> pace(Metric paceMetric, Metric speedMetric) const {
        if (const auto pace = value(paceMetric)) return pace;
        if (const auto speed = value(speedMetric)) return *speed > 0.0F ? 1800.0F / *speed : 0.0F;
        return std::nullopt;
    }

private:
    const TrainerState& state_;
};

// s/500 m <-> FTMS treadmill pace in 0.1 km/min (0 = not moving).
std::optional<float> treadmillPace(std::optional<float> secondsPer500m) {
    if (!secondsPer500m) return std::nullopt;
    return *secondsPer500m > 0.0F ? 30.0F / *secondsPer500m : 0.0F;
}

}  // namespace

IndoorBikeData indoorBikeDataFrom(const TrainerState& state) {
    const StateReader in(state);
    IndoorBikeData data;
    data.instantaneousSpeed = in.u16(Metric::Speed, 100.0F);
    data.averageSpeed = in.u16(Metric::AverageSpeed, 100.0F);
    data.instantaneousCadence = in.u16(Metric::Cadence, 2.0F);
    data.averageCadence = in.u16(Metric::AverageCadence, 2.0F);
    data.totalDistance = in.u24(Metric::Distance, 1.0F);
    data.resistanceLevel = in.s16(Metric::Resistance, 1.0F);
    data.instantaneousPower = in.s16(Metric::Power, 1.0F);
    data.averagePower = in.s16(Metric::AveragePower, 1.0F);
    in.common(data);
    return data;
}

TreadmillData treadmillDataFrom(const TrainerState& state) {
    const StateReader in(state);
    TreadmillData data;
    data.instantaneousSpeed = in.u16(Metric::Speed, 100.0F);
    data.averageSpeed = in.u16(Metric::AverageSpeed, 100.0F);
    data.totalDistance = in.u24(Metric::Distance, 1.0F);
    data.inclination = StateReader::s16(in.incline(), 10.0F);
    if (data.inclination) data.rampAngleSetting = StateReader::s16(in.rampAngle(), 10.0F);
    data.positiveElevationGain = in.u16(Metric::PositiveElevationGain, 10.0F);
    data.negativeElevationGain = in.u16(Metric::NegativeElevationGain, 10.0F);
    data.instantaneousPace = StateReader::u8(treadmillPace(in.value(Metric::Pace)), 10.0F);
    data.averagePace = StateReader::u8(treadmillPace(in.value(Metric::AveragePace)), 10.0F);
    in.common(data);
    data.forceOnBelt = in.s16(Metric::ForceOnBelt, 1.0F);
    data.powerOutput = in.s16(Metric::Power, 1.0F);
    return data;
}

CrossTrainerData crossTrainerDataFrom(const TrainerState& state) {
    const StateReader in(state);
    CrossTrainerData data;
    data.instantaneousSpeed = in.u16(Metric::Speed, 100.0F);
    data.averageSpeed = in.u16(Metric::AverageSpeed, 100.0F);
    data.totalDistance = in.u24(Metric::Distance, 1.0F);
    data.stepPerMinute = in.u16(Metric::Cadence, 1.0F);
    data.averageStepRate = in.u16(Metric::AverageCadence, 1.0F);
    data.strideCount = in.u16(Metric::StrideCount, 10.0F);
    data.positiveElevationGain = in.u16(Metric::PositiveElevationGain, 1.0F);
    data.negativeElevationGain = in.u16(Metric::NegativeElevationGain, 1.0F);
    data.inclination = StateReader::s16(in.incline(), 10.0F);
    if (data.inclination) data.rampAngleSetting = StateReader::s16(in.rampAngle(), 10.0F);
    data.resistanceLevel = in.s16(Metric::Resistance, 10.0F);
    data.instantaneousPower = in.s16(Metric::Power, 1.0F);
    data.averagePower = in.s16(Metric::AveragePower, 1.0F);
    in.common(data);
    return data;
}

StepClimberData stepClimberDataFrom(const TrainerState& state) {
    const StateReader in(state);
    StepClimberData data;
    data.floors = in.u16(Metric::Floors, 1.0F);
    data.stepCount = in.u16(Metric::StepCount, 1.0F);
    data.stepPerMinute = in.u16(Metric::Cadence, 1.0F);
    data.averageStepRate = in.u16(Metric::AverageCadence, 1.0F);
    data.positiveElevationGain = in.u16(Metric::PositiveElevationGain, 1.0F);
    in.common(data);
    return data;
}

StairClimberData stairClimberDataFrom(const TrainerState& state) {
    const StateReader in(state);
    StairClimberData data;
    data.floors = in.u16(Metric::Floors, 1.0F);
    data.stepPerMinute = in.u16(Metric::Cadence, 1.0F);
    data.averageStepRate = in.u16(Metric::AverageCadence, 1.0F);
    data.positiveElevationGain = in.u16(Metric::PositiveElevationGain, 1.0F);
    data.strideCount = in.u16(Metric::StrideCount, 1.0F);
    in.common(data);
    return data;
}

RowerData rowerDataFrom(const TrainerState& state) {
    const StateReader in(state);
    RowerData data;
    data.strokeRate = in.u8(Metric::Cadence, 2.0F);
    data.strokeCount = in.u16(Metric::StrokeCount, 1.0F);
    data.averageStrokeRate = in.u8(Metric::AverageCadence, 2.0F);
    data.totalDistance = in.u24(Metric::Distance, 1.0F);
    data.instantaneousPace = StateReader::u16(in.pace(Metric::Pace, Metric::Speed), 1.0F);
    data.averagePace = StateReader::u16(in.pace(Metric::AveragePace, Metric::AverageSpeed), 1.0F);
    data.instantaneousPower = in.s16(Metric::Power, 1.0F);
    data.averagePower = in.s16(Metric::AveragePower, 1.0F);
    data.resistanceLevel = in.s16(Metric::Resistance, 1.0F);
    in.common(data);
    return data;
}

std::uint16_t machineDataUuid(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill: return kTreadmillDataUuid;
        case MachineProfile::CrossTrainer: return kCrossTrainerDataUuid;
        case MachineProfile::StepClimber: return kStepClimberDataUuid;
        case MachineProfile::StairClimber: return kStairClimberDataUuid;
        case MachineProfile::Rower: return kRowerDataUuid;
        case MachineProfile::IndoorBike:
        default: return kIndoorBikeDataUuid;
    }
}

std::vector<Bytes> encodeMachineDataRecords(MachineProfile profile, const TrainerState& state, std::size_t maxLength) {
    switch (profile) {
        case MachineProfile::Treadmill: return encodeRecords(treadmillDataFrom(state), maxLength);
        case MachineProfile::CrossTrainer: return encodeRecords(crossTrainerDataFrom(state), maxLength);
        case MachineProfile::StepClimber: return encodeRecords(stepClimberDataFrom(state), maxLength);
        case MachineProfile::StairClimber: return encodeRecords(stairClimberDataFrom(state), maxLength);
        case MachineProfile::Rower: return encodeRecords(rowerDataFrom(state), maxLength);
        case MachineProfile::IndoorBike:
        default: return encodeRecords(indoorBikeDataFrom(state), maxLength);
    }
}

Bytes encodeMachineData(MachineProfile profile, const TrainerState& state) {
    switch (profile) {
        case MachineProfile::Treadmill: return encodeTreadmillData(treadmillDataFrom(state));
        case MachineProfile::CrossTrainer: return encodeCrossTrainerData(crossTrainerDataFrom(state));
        case MachineProfile::StepClimber: return encodeStepClimberData(stepClimberDataFrom(state));
        case MachineProfile::StairClimber: return encodeStairClimberData(stairClimberDataFrom(state));
        case MachineProfile::Rower: return encodeRowerData(rowerDataFrom(state));
        case MachineProfile::IndoorBike:
        default: return encodeIndoorBikeData(indoorBikeDataFrom(state));
    }
}

bool profileAcceptsControl(MachineProfile profile, Control control) {
    switch (control) {
        case Control::Speed:
            return profile == MachineProfile::Treadmill || profile == MachineProfile::CrossTrainer;
        case Control::Incline:
            return profile == MachineProfile::IndoorBike || profile == MachineProfile::Treadmill ||
                   profile == MachineProfile::CrossTrainer;
        case Control::Resistance:
        case Control::Power:
            return profile != MachineProfile::Treadmill;
        default: return false;
    }
}

Bytes encodeFeature(const TrainerCapabilities& capabilities, MachineProfile profile) {
    const auto has = [&capabilities](std::initializer_list<Metric> metrics) {
        for (const Metric metric : metrics)
            if (capabilities.supports(metric)) return true;
        return false;
    };
    const bool bike = profile == MachineProfile::IndoorBike;
    const bool treadmill = profile == MachineProfile::Treadmill;
    const bool cross = profile == MachineProfile::CrossTrainer;
    const bool rower = profile == MachineProfile::Rower;
    const bool climber = profile == MachineProfile::StepClimber || profile == MachineProfile::StairClimber;

    // Only features the profile's data characteristic can actually carry.
    std::uint32_t features = 0;
    if ((bike || treadmill || cross) && has({Metric::AverageSpeed})) features |= kFeatureAverageSpeed;
    if (rower && has({Metric::AveragePace, Metric::AverageSpeed})) features |= kFeatureAverageSpeed;
    if ((bike || rower) && has({Metric::Cadence})) features |= kFeatureCadence;
    if (!climber && has({Metric::Distance})) features |= kFeatureTotalDistance;
    // Bikes take inclination through the indoor bike simulation target.
    if (bike && has({Metric::Incline})) features |= kFeatureInclination;
    if ((treadmill || cross) && has({Metric::Incline, Metric::RampAngle})) features |= kFeatureInclination;
    if ((treadmill || cross) && has({Metric::PositiveElevationGain, Metric::NegativeElevationGain}))
        features |= kFeatureElevationGain;
    if (climber && has({Metric::PositiveElevationGain})) features |= kFeatureElevationGain;
    if (treadmill && has({Metric::Pace, Metric::AveragePace})) features |= kFeaturePace;
    if (rower && has({Metric::Pace, Metric::Speed})) features |= kFeaturePace;
    if (cross && has({Metric::Cadence})) features |= kFeatureStepCount;
    if (climber && has({Metric::Cadence, Metric::StepCount, Metric::Floors})) features |= kFeatureStepCount;
    if ((bike || cross || rower) && has({Metric::Resistance})) features |= kFeatureResistanceLevel;
    if ((cross || profile == MachineProfile::StairClimber) && has({Metric::StrideCount}))
        features |= kFeatureStrideCount;
    if (has({Metric::TotalEnergy, Metric::EnergyPerHour, Metric::EnergyPerMinute})) features |= kFeatureExpendedEnergy;
    if (has({Metric::HeartRate})) features |= kFeatureHeartRate;
    if (has({Metric::MetabolicEquivalent})) features |= kFeatureMetabolicEquivalent;
    if (has({Metric::ElapsedTime})) features |= kFeatureElapsedTime;
    if (has({Metric::RemainingTime})) features |= kFeatureRemainingTime;
    if ((bike || cross || rower) && has({Metric::Power})) features |= kFeaturePower;
    if (treadmill && has({Metric::Power, Metric::ForceOnBelt})) features |= kFeaturePowerAndForce;

    const auto accepts = [&capabilities, profile](Control control) {
        return capabilities.supports(control) && profileAcceptsControl(profile, control);
    };
    std::uint32_t targets = 0;
    if (accepts(Control::Speed)) targets |= kTargetSpeed;
    if (accepts(Control::Incline)) targets |= kTargetInclination;
    if (bike && accepts(Control::Incline)) targets |= kTargetIndoorBikeSimulation;
    if (accepts(Control::Resistance)) targets |= kTargetResistance;
    if (accepts(Control::Power)) targets |= kTargetPower;

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
    // Flags: machine available; then the little-endian Fitness Machine Type.
    std::uint16_t type = 1U << 5;  // indoor bike
    switch (profile) {
        case MachineProfile::Treadmill: type = 1U << 0; break;
        case MachineProfile::CrossTrainer: type = 1U << 1; break;
        case MachineProfile::StepClimber: type = 1U << 2; break;
        case MachineProfile::StairClimber: type = 1U << 3; break;
        case MachineProfile::Rower: type = 1U << 4; break;
        case MachineProfile::IndoorBike: break;
    }
    return {0x01, static_cast<std::uint8_t>(type & 0xFFU), static_cast<std::uint8_t>(type >> 8)};
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
            if (!profileAcceptsControl(profile, Control::Speed)) break;
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
            if (!profileAcceptsControl(profile, Control::Incline)) break;
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
            if (!profileAcceptsControl(profile, Control::Resistance)) break;
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
            if (!profileAcceptsControl(profile, Control::Power)) break;
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

template <typename T>
void read(FieldReader& reader, std::optional<T>& out, std::size_t width) {
    const std::uint32_t value = reader.uint(width);
    if (reader.ok()) out = static_cast<T>(value);
}

template <typename Data>
void readEnergy(FieldReader& reader, Data& out) {
    read(reader, out.totalEnergy, 2);
    read(reader, out.energyPerHour, 2);
    read(reader, out.energyPerMinute, 1);
    // "Data not available" is not a measurement.
    if (out.totalEnergy == kEnergyNotAvailable16) out.totalEnergy.reset();
    if (out.energyPerHour == kEnergyNotAvailable16) out.energyPerHour.reset();
    if (out.energyPerMinute == kEnergyNotAvailable8) out.energyPerMinute.reset();
}

template <typename Data>
bool finish(const FieldReader& reader, Data& out) {
    if (!reader.ok()) out = {};
    return reader.ok();
}

// Input mapping helpers: wire value * resolution -> normalized metric.
template <typename T>
void map(MachineData& out, Metric metric, const std::optional<T>& value, float resolution = 1.0F) {
    if (value) out[metric] = {static_cast<float>(*value) * resolution, true};
}

template <typename Data>
void mapCommon(MachineData& out, const Data& data) {
    map(out, Metric::TotalEnergy, data.totalEnergy);
    map(out, Metric::EnergyPerHour, data.energyPerHour);
    map(out, Metric::EnergyPerMinute, data.energyPerMinute);
    map(out, Metric::HeartRate, data.heartRate);
    map(out, Metric::MetabolicEquivalent, data.metabolicEquivalent, 0.1F);
    map(out, Metric::ElapsedTime, data.elapsedTime);
    map(out, Metric::RemainingTime, data.remainingTime);
}

// FTMS treadmill pace (0.1 km/min) -> s/500 m; 0 means not moving.
void mapTreadmillPace(MachineData& out, Metric metric, const std::optional<std::uint8_t>& value) {
    if (value) out[metric] = {*value == 0 ? 0.0F : 300.0F / static_cast<float>(*value), true};
}

// Rower pace (s/500 m) -> speed in km/h; 0 means not moving.
void mapRowerSpeed(MachineData& out, Metric metric, const std::optional<std::uint16_t>& pace) {
    if (pace) out[metric] = {*pace == 0 ? 0.0F : 1800.0F / static_cast<float>(*pace), true};
}

}  // namespace

bool decodeIndoorBikeData(const std::uint8_t* data, std::size_t length, IndoorBikeData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) read(reader, out.instantaneousSpeed, 2);
    if (flags & (1U << 1)) read(reader, out.averageSpeed, 2);
    if (flags & (1U << 2)) read(reader, out.instantaneousCadence, 2);
    if (flags & (1U << 3)) read(reader, out.averageCadence, 2);
    if (flags & (1U << 4)) read(reader, out.totalDistance, 3);
    if (flags & (1U << 5)) read(reader, out.resistanceLevel, 2);
    if (flags & (1U << 6)) read(reader, out.instantaneousPower, 2);
    if (flags & (1U << 7)) read(reader, out.averagePower, 2);
    if (flags & (1U << 8)) readEnergy(reader, out);
    if (flags & (1U << 9)) read(reader, out.heartRate, 1);
    if (flags & (1U << 10)) read(reader, out.metabolicEquivalent, 1);
    if (flags & (1U << 11)) read(reader, out.elapsedTime, 2);
    if (flags & (1U << 12)) read(reader, out.remainingTime, 2);
    return finish(reader, out);
}

bool decodeTreadmillData(const std::uint8_t* data, std::size_t length, TreadmillData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) read(reader, out.instantaneousSpeed, 2);
    if (flags & (1U << 1)) read(reader, out.averageSpeed, 2);
    if (flags & (1U << 2)) read(reader, out.totalDistance, 3);
    if (flags & (1U << 3)) {
        read(reader, out.inclination, 2);
        read(reader, out.rampAngleSetting, 2);
    }
    if (flags & (1U << 4)) {
        read(reader, out.positiveElevationGain, 2);
        read(reader, out.negativeElevationGain, 2);
    }
    if (flags & (1U << 5)) read(reader, out.instantaneousPace, 1);
    if (flags & (1U << 6)) read(reader, out.averagePace, 1);
    if (flags & (1U << 7)) readEnergy(reader, out);
    if (flags & (1U << 8)) read(reader, out.heartRate, 1);
    if (flags & (1U << 9)) read(reader, out.metabolicEquivalent, 1);
    if (flags & (1U << 10)) read(reader, out.elapsedTime, 2);
    if (flags & (1U << 11)) read(reader, out.remainingTime, 2);
    if (flags & (1U << 12)) {
        read(reader, out.forceOnBelt, 2);
        read(reader, out.powerOutput, 2);
    }
    return finish(reader, out);
}

bool decodeCrossTrainerData(const std::uint8_t* data, std::size_t length, CrossTrainerData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(3);
    if (!(flags & (1U << 0))) read(reader, out.instantaneousSpeed, 2);
    if (flags & (1U << 1)) read(reader, out.averageSpeed, 2);
    if (flags & (1U << 2)) read(reader, out.totalDistance, 3);
    if (flags & (1U << 3)) {
        read(reader, out.stepPerMinute, 2);
        read(reader, out.averageStepRate, 2);
    }
    if (flags & (1U << 4)) read(reader, out.strideCount, 2);
    if (flags & (1U << 5)) {
        read(reader, out.positiveElevationGain, 2);
        read(reader, out.negativeElevationGain, 2);
    }
    if (flags & (1U << 6)) {
        read(reader, out.inclination, 2);
        read(reader, out.rampAngleSetting, 2);
    }
    if (flags & (1U << 7)) read(reader, out.resistanceLevel, 2);
    if (flags & (1U << 8)) read(reader, out.instantaneousPower, 2);
    if (flags & (1U << 9)) read(reader, out.averagePower, 2);
    if (flags & (1U << 10)) readEnergy(reader, out);
    if (flags & (1U << 11)) read(reader, out.heartRate, 1);
    if (flags & (1U << 12)) read(reader, out.metabolicEquivalent, 1);
    if (flags & (1U << 13)) read(reader, out.elapsedTime, 2);
    if (flags & (1U << 14)) read(reader, out.remainingTime, 2);
    out.movementBackward = (flags & (1U << 15)) != 0;
    return finish(reader, out);
}

bool decodeStepClimberData(const std::uint8_t* data, std::size_t length, StepClimberData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) {
        read(reader, out.floors, 2);
        read(reader, out.stepCount, 2);
    }
    if (flags & (1U << 1)) read(reader, out.stepPerMinute, 2);
    if (flags & (1U << 2)) read(reader, out.averageStepRate, 2);
    if (flags & (1U << 3)) read(reader, out.positiveElevationGain, 2);
    if (flags & (1U << 4)) readEnergy(reader, out);
    if (flags & (1U << 5)) read(reader, out.heartRate, 1);
    if (flags & (1U << 6)) read(reader, out.metabolicEquivalent, 1);
    if (flags & (1U << 7)) read(reader, out.elapsedTime, 2);
    if (flags & (1U << 8)) read(reader, out.remainingTime, 2);
    return finish(reader, out);
}

bool decodeStairClimberData(const std::uint8_t* data, std::size_t length, StairClimberData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) read(reader, out.floors, 2);
    if (flags & (1U << 1)) read(reader, out.stepPerMinute, 2);
    if (flags & (1U << 2)) read(reader, out.averageStepRate, 2);
    if (flags & (1U << 3)) read(reader, out.positiveElevationGain, 2);
    if (flags & (1U << 4)) read(reader, out.strideCount, 2);
    if (flags & (1U << 5)) readEnergy(reader, out);
    if (flags & (1U << 6)) read(reader, out.heartRate, 1);
    if (flags & (1U << 7)) read(reader, out.metabolicEquivalent, 1);
    if (flags & (1U << 8)) read(reader, out.elapsedTime, 2);
    if (flags & (1U << 9)) read(reader, out.remainingTime, 2);
    return finish(reader, out);
}

bool decodeRowerData(const std::uint8_t* data, std::size_t length, RowerData& out) {
    out = {};
    FieldReader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    if (!(flags & (1U << 0))) {
        read(reader, out.strokeRate, 1);
        read(reader, out.strokeCount, 2);
    }
    if (flags & (1U << 1)) read(reader, out.averageStrokeRate, 1);
    if (flags & (1U << 2)) read(reader, out.totalDistance, 3);
    if (flags & (1U << 3)) read(reader, out.instantaneousPace, 2);
    if (flags & (1U << 4)) read(reader, out.averagePace, 2);
    if (flags & (1U << 5)) read(reader, out.instantaneousPower, 2);
    if (flags & (1U << 6)) read(reader, out.averagePower, 2);
    if (flags & (1U << 7)) read(reader, out.resistanceLevel, 2);
    if (flags & (1U << 8)) readEnergy(reader, out);
    if (flags & (1U << 9)) read(reader, out.heartRate, 1);
    if (flags & (1U << 10)) read(reader, out.metabolicEquivalent, 1);
    if (flags & (1U << 11)) read(reader, out.elapsedTime, 2);
    if (flags & (1U << 12)) read(reader, out.remainingTime, 2);
    return finish(reader, out);
}

MachineData toMachineData(const IndoorBikeData& data) {
    MachineData out;
    map(out, Metric::Speed, data.instantaneousSpeed, 0.01F);
    map(out, Metric::AverageSpeed, data.averageSpeed, 0.01F);
    map(out, Metric::Cadence, data.instantaneousCadence, 0.5F);
    map(out, Metric::AverageCadence, data.averageCadence, 0.5F);
    map(out, Metric::Distance, data.totalDistance);
    map(out, Metric::Resistance, data.resistanceLevel);
    map(out, Metric::Power, data.instantaneousPower);
    map(out, Metric::AveragePower, data.averagePower);
    mapCommon(out, data);
    return out;
}

MachineData toMachineData(const TreadmillData& data) {
    MachineData out;
    map(out, Metric::Speed, data.instantaneousSpeed, 0.01F);
    map(out, Metric::AverageSpeed, data.averageSpeed, 0.01F);
    map(out, Metric::Distance, data.totalDistance);
    map(out, Metric::Incline, data.inclination, 0.1F);
    map(out, Metric::RampAngle, data.rampAngleSetting, 0.1F);
    map(out, Metric::PositiveElevationGain, data.positiveElevationGain, 0.1F);
    map(out, Metric::NegativeElevationGain, data.negativeElevationGain, 0.1F);
    mapTreadmillPace(out, Metric::Pace, data.instantaneousPace);
    mapTreadmillPace(out, Metric::AveragePace, data.averagePace);
    mapCommon(out, data);
    map(out, Metric::ForceOnBelt, data.forceOnBelt);
    map(out, Metric::Power, data.powerOutput);
    return out;
}

MachineData toMachineData(const CrossTrainerData& data) {
    MachineData out;
    map(out, Metric::Speed, data.instantaneousSpeed, 0.01F);
    map(out, Metric::AverageSpeed, data.averageSpeed, 0.01F);
    map(out, Metric::Distance, data.totalDistance);
    map(out, Metric::Cadence, data.stepPerMinute);
    map(out, Metric::AverageCadence, data.averageStepRate);
    map(out, Metric::StrideCount, data.strideCount, 0.1F);
    map(out, Metric::PositiveElevationGain, data.positiveElevationGain);
    map(out, Metric::NegativeElevationGain, data.negativeElevationGain);
    map(out, Metric::Incline, data.inclination, 0.1F);
    map(out, Metric::RampAngle, data.rampAngleSetting, 0.1F);
    map(out, Metric::Resistance, data.resistanceLevel, 0.1F);
    map(out, Metric::Power, data.instantaneousPower);
    map(out, Metric::AveragePower, data.averagePower);
    mapCommon(out, data);
    return out;
}

MachineData toMachineData(const StepClimberData& data) {
    MachineData out;
    map(out, Metric::Floors, data.floors);
    map(out, Metric::StepCount, data.stepCount);
    map(out, Metric::Cadence, data.stepPerMinute);
    map(out, Metric::AverageCadence, data.averageStepRate);
    map(out, Metric::PositiveElevationGain, data.positiveElevationGain);
    mapCommon(out, data);
    return out;
}

MachineData toMachineData(const StairClimberData& data) {
    MachineData out;
    map(out, Metric::Floors, data.floors);
    map(out, Metric::Cadence, data.stepPerMinute);
    map(out, Metric::AverageCadence, data.averageStepRate);
    map(out, Metric::PositiveElevationGain, data.positiveElevationGain);
    map(out, Metric::StrideCount, data.strideCount);
    mapCommon(out, data);
    return out;
}

MachineData toMachineData(const RowerData& data) {
    MachineData out;
    map(out, Metric::Cadence, data.strokeRate, 0.5F);
    map(out, Metric::StrokeCount, data.strokeCount);
    map(out, Metric::AverageCadence, data.averageStrokeRate, 0.5F);
    map(out, Metric::Distance, data.totalDistance);
    map(out, Metric::Pace, data.instantaneousPace);
    map(out, Metric::AveragePace, data.averagePace);
    mapRowerSpeed(out, Metric::Speed, data.instantaneousPace);
    mapRowerSpeed(out, Metric::AverageSpeed, data.averagePace);
    map(out, Metric::Power, data.instantaneousPower);
    map(out, Metric::AveragePower, data.averagePower);
    map(out, Metric::Resistance, data.resistanceLevel);
    mapCommon(out, data);
    return out;
}

namespace {
template <typename Data>
bool decodeAndMap(bool (*decode)(const std::uint8_t*, std::size_t, Data&), const std::uint8_t* data,
                  std::size_t length, MachineData& out) {
    Data decoded;
    const bool ok = decode(data, length, decoded);
    out = ok ? toMachineData(decoded) : MachineData{};
    return ok;
}
}  // namespace

bool decodeIndoorBikeData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeAndMap<IndoorBikeData>(decodeIndoorBikeData, data, length, out);
}
bool decodeTreadmillData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeAndMap<TreadmillData>(decodeTreadmillData, data, length, out);
}
bool decodeCrossTrainerData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeAndMap<CrossTrainerData>(decodeCrossTrainerData, data, length, out);
}
bool decodeStepClimberData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeAndMap<StepClimberData>(decodeStepClimberData, data, length, out);
}
bool decodeStairClimberData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeAndMap<StairClimberData>(decodeStairClimberData, data, length, out);
}
bool decodeRowerData(const std::uint8_t* data, std::size_t length, MachineData& out) {
    return decodeAndMap<RowerData>(decodeRowerData, data, length, out);
}

bool decodeMachineData(MachineProfile profile, const std::uint8_t* data, std::size_t length, MachineData& out) {
    switch (profile) {
        case MachineProfile::Treadmill: return decodeTreadmillData(data, length, out);
        case MachineProfile::CrossTrainer: return decodeCrossTrainerData(data, length, out);
        case MachineProfile::StepClimber: return decodeStepClimberData(data, length, out);
        case MachineProfile::StairClimber: return decodeStairClimberData(data, length, out);
        case MachineProfile::Rower: return decodeRowerData(data, length, out);
        case MachineProfile::IndoorBike:
        default: return decodeIndoorBikeData(data, length, out);
    }
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
