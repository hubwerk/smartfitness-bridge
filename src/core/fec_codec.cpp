#include "ant/fec_codec.hpp"

#include <cmath>
#include <optional>

namespace smartfitness {
namespace ant {
namespace fec {
namespace {

constexpr float kPi = 3.14159265358979323846F;

std::uint32_t clampRound(float value, std::uint32_t maximum) {
    if (!std::isfinite(value) || value <= 0.0F) return 0;
    const float rounded = std::round(value);
    return rounded >= static_cast<float>(maximum) ? maximum : static_cast<std::uint32_t>(rounded);
}

// Rollover counters (distance, strokes, vertical distance) wrap like the
// display expects: the low byte of the accumulated value.
std::uint8_t rollover8(float value) {
    if (!std::isfinite(value) || value <= 0.0F) return 0;
    return static_cast<std::uint8_t>(static_cast<std::uint64_t>(std::floor(value)) & 0xFFU);
}

void put16(Page& page, std::size_t index, std::uint16_t value) {
    page[index] = static_cast<std::uint8_t>(value & 0xFFU);
    page[index + 1] = static_cast<std::uint8_t>(value >> 8);
}

std::uint16_t get16(const Page& page, std::size_t index) {
    return static_cast<std::uint16_t>(page[index] | (page[index + 1] << 8));
}

class Values {
public:
    explicit Values(const TrainerState& state) : state_(state) {}
    std::optional<float> get(Metric metric) const {
        if (!state_.connected) return std::nullopt;
        const auto& sample = state_.sample(metric);
        if (!sample.valid || !std::isfinite(sample.value)) return std::nullopt;
        return sample.value;
    }
    // m/s from the speed, else from the pace (s/500 m).
    std::optional<float> speedMps() const {
        if (const auto speed = get(Metric::Speed)) return *speed / 3.6F;
        if (const auto pace = get(Metric::Pace)) return *pace > 0.0F ? 500.0F / *pace : 0.0F;
        return std::nullopt;
    }
    // Grade in % from the incline, else from the ramp angle.
    std::optional<float> incline() const {
        if (const auto grade = get(Metric::Incline)) return grade;
        if (const auto angle = get(Metric::RampAngle)) return std::tan(*angle * kPi / 180.0F) * 100.0F;
        return std::nullopt;
    }
    // kcal/h from the hourly rate, else from the per-minute rate.
    std::optional<float> caloriesPerHour() const {
        if (const auto rate = get(Metric::EnergyPerHour)) return rate;
        if (const auto rate = get(Metric::EnergyPerMinute)) return *rate * 60.0F;
        return std::nullopt;
    }
    std::uint8_t cadence() const {
        const auto cadence = get(Metric::Cadence);
        return cadence ? static_cast<std::uint8_t>(clampRound(*cadence, 254)) : 0xFF;
    }
    std::uint16_t power(std::uint16_t invalid) const {
        const auto power = get(Metric::Power);
        return power ? static_cast<std::uint16_t>(clampRound(*power, invalid - 1U)) : invalid;
    }

private:
    const TrainerState& state_;
};

Page blank(std::uint8_t number) {
    Page page;
    page.fill(0xFF);
    page[0] = number;
    return page;
}

}  // namespace

std::uint8_t equipmentType(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill: return 19;
        case MachineProfile::CrossTrainer: return 20;
        case MachineProfile::Rower: return 22;
        case MachineProfile::StepClimber:
        case MachineProfile::StairClimber: return 23;
        case MachineProfile::IndoorBike: break;
    }
    return 25;
}

std::uint8_t specificPage(MachineProfile profile) {
    switch (profile) {
        case MachineProfile::Treadmill: return page::kTreadmill;
        case MachineProfile::CrossTrainer: return page::kElliptical;
        case MachineProfile::Rower: return page::kRower;
        case MachineProfile::StepClimber:
        case MachineProfile::StairClimber: return page::kClimber;
        case MachineProfile::IndoorBike: break;
    }
    return page::kTrainer;
}

CommandStatus commandStatusFor(ControlResult result) {
    switch (result) {
        case ControlResult::Applied: return CommandStatus::Pass;
        case ControlResult::Unsupported: return CommandStatus::NotSupported;
        case ControlResult::InvalidValue: return CommandStatus::Rejected;
        default: return CommandStatus::Fail;
    }
}

ControlPage decodeControlPage(const Page& data) {
    ControlPage out;
    switch (data[0]) {
        case page::kBasicResistance:
            out.known = true;
            out.hasRequest = true;
            out.request = {Control::Resistance, static_cast<float>(data[7]) * 0.5F};
            break;
        case page::kTargetPower:
            out.known = true;
            out.hasRequest = true;
            out.request = {Control::Power, static_cast<float>(get16(data, 6)) * 0.25F};
            break;
        case page::kTrackResistance: {
            out.known = true;
            out.hasRequest = true;
            const std::uint16_t raw = get16(data, 5);
            // 0xFFFF: use the default grade (0 %).
            out.request = {Control::Incline, raw == 0xFFFF ? 0.0F : static_cast<float>(raw) * 0.01F - 200.0F};
            break;
        }
        case page::kWindResistance:
        case page::kUserConfiguration:
            out.known = true;  // accepted; the source has no matching parameter
            break;
        default: break;
    }
    return out;
}

void Encoder::reset() { *this = Encoder{}; }

void Encoder::update(const TrainerState& state, MachineProfile profile, std::uint32_t nowMs) {
    if (clockStarted_ && state_.connected && state.connected) localElapsedMs_ += nowMs - lastMs_;
    if (!state.connected) localElapsedMs_ = 0;
    clockStarted_ = true;
    lastMs_ = nowMs;
    state_ = state;
    profile_ = profile;
}

FeState Encoder::state() const {
    const Values in(state_);
    if (!state_.connected) return FeState::Ready;
    const auto speed = in.speedMps();
    const auto cadence = in.get(Metric::Cadence);
    const auto power = in.get(Metric::Power);
    const bool moving = (speed && *speed > 0.0F) || (cadence && *cadence > 0.0F) || (power && *power > 0.0F);
    return moving ? FeState::InUse : FeState::Ready;
}

std::uint8_t Encoder::stateBits() const { return static_cast<std::uint8_t>(static_cast<std::uint8_t>(state()) << 4); }

Page Encoder::generalFeData() const {
    const Values in(state_);
    Page out = blank(page::kGeneralFe);
    out[1] = equipmentType(profile_);
    const auto elapsed = in.get(Metric::ElapsedTime);
    const float seconds = elapsed ? *elapsed : static_cast<float>(localElapsedMs_) / 1000.0F;
    out[2] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(std::floor(std::fmax(seconds, 0.0F) * 4.0F)) & 0xFFU);
    std::uint8_t capabilities = 0;
    const auto distance = in.get(Metric::Distance);
    out[3] = distance ? rollover8(*distance) : 0;
    if (distance) capabilities |= 0x04;
    const auto speed = in.speedMps();
    put16(out, 4, speed ? static_cast<std::uint16_t>(clampRound(*speed * 1000.0F, 0xFFFE)) : 0xFFFF);
    const auto heartRate = in.get(Metric::HeartRate);
    out[6] = heartRate ? static_cast<std::uint8_t>(clampRound(*heartRate, 254)) : 0xFF;
    if (heartRate) capabilities |= 0x03;  // hand contact sensor (reported by the machine)
    out[7] = static_cast<std::uint8_t>(capabilities | stateBits());
    return out;
}

bool Encoder::hasSettings() const {
    const Values in(state_);
    return in.incline() || in.get(Metric::Resistance);
}

Page Encoder::generalSettings() const {
    const Values in(state_);
    Page out = blank(page::kGeneralSettings);
    const auto incline = in.incline();
    if (incline) {
        const float scaled = std::round(*incline * 100.0F);
        const auto value = static_cast<std::int16_t>(std::fmax(-32767.0F, std::fmin(32766.0F, scaled)));
        put16(out, 4, static_cast<std::uint16_t>(value));
    } else {
        put16(out, 4, 0x7FFF);
    }
    const auto resistance = in.get(Metric::Resistance);
    out[6] = resistance ? static_cast<std::uint8_t>(clampRound(std::fmin(*resistance, 100.0F) * 2.0F, 200)) : 0xFF;
    out[7] = stateBits();
    return out;
}

bool Encoder::hasMetabolic() const {
    const Values in(state_);
    return in.get(Metric::MetabolicEquivalent) || in.caloriesPerHour() || in.get(Metric::TotalEnergy);
}

Page Encoder::metabolic() const {
    const Values in(state_);
    Page out = blank(page::kMetabolic);
    const auto met = in.get(Metric::MetabolicEquivalent);
    put16(out, 2, met ? static_cast<std::uint16_t>(clampRound(*met * 100.0F, 0xFFFE)) : 0xFFFF);
    const auto rate = in.caloriesPerHour();
    put16(out, 4, rate ? static_cast<std::uint16_t>(clampRound(*rate * 10.0F, 0xFFFE)) : 0xFFFF);
    const auto total = in.get(Metric::TotalEnergy);
    out[6] = total ? rollover8(*total) : 0;
    out[7] = static_cast<std::uint8_t>((total ? 0x01 : 0x00) | stateBits());
    return out;
}

Page Encoder::specific() {
    const Values in(state_);
    const std::uint8_t number = specificPage(profile_);
    Page out = blank(number);
    const auto positive = in.get(Metric::PositiveElevationGain);
    switch (number) {
        case page::kTreadmill: {
            out[4] = in.cadence();
            const auto negative = in.get(Metric::NegativeElevationGain);
            out[5] = negative ? rollover8(*negative * 10.0F) : 0;
            out[6] = positive ? rollover8(*positive * 10.0F) : 0;
            out[7] = static_cast<std::uint8_t>((negative ? 0x01 : 0) | (positive ? 0x02 : 0) | stateBits());
            break;
        }
        case page::kElliptical: {
            const auto strides = in.get(Metric::StrideCount);
            out[2] = strides ? rollover8(*strides) : 0;
            out[3] = positive ? rollover8(*positive * 10.0F) : 0;
            out[4] = in.cadence();
            put16(out, 5, in.power(0xFFFF));
            out[7] = static_cast<std::uint8_t>((positive ? 0x01 : 0) | stateBits());
            break;
        }
        case page::kRower: {
            const auto strokes = in.get(Metric::StrokeCount);
            out[3] = strokes ? rollover8(*strokes) : 0;
            out[4] = in.cadence();
            put16(out, 5, in.power(0xFFFF));
            out[7] = stateBits();
            break;
        }
        case page::kClimber: {
            auto strides = in.get(Metric::StrideCount);
            if (!strides) strides = in.get(Metric::StepCount);
            out[3] = strides ? rollover8(*strides) : 0;
            out[4] = in.cadence();
            put16(out, 5, in.power(0xFFFF));
            out[7] = stateBits();
            break;
        }
        default: {  // trainer / stationary bike
            const std::uint16_t power = in.power(0x0FFF);
            ++eventCount_;
            if (power != 0x0FFF) accumulatedPower_ = static_cast<std::uint16_t>(accumulatedPower_ + power);
            out[1] = eventCount_;
            out[2] = in.cadence();
            put16(out, 3, accumulatedPower_);
            out[5] = static_cast<std::uint8_t>(power & 0xFFU);
            out[6] = static_cast<std::uint8_t>((power >> 8) & 0x0FU);  // trainer status bits 4-7: none
            out[7] = stateBits();  // target power flags 0: at target / no target
            break;
        }
    }
    return out;
}

Page Encoder::capabilities() const {
    Page out = blank(page::kCapabilities);
    put16(out, 5, 0);  // maximum resistance unknown
    const auto& caps = state_.capabilities;
    const bool connected = state_.connected && state_.trainerDetected;
    out[7] = static_cast<std::uint8_t>((connected && caps.supports(Control::Resistance) ? 0x01 : 0) |
                                       (connected && caps.supports(Control::Power) ? 0x02 : 0) |
                                       (connected && caps.supports(Control::Incline) ? 0x04 : 0));
    return out;
}

Page Encoder::commandStatus() const {
    Page out = blank(page::kCommandStatus);
    out[1] = commandPage_;
    out[2] = commandSequence_;
    out[3] = static_cast<std::uint8_t>(commandStatus_);
    for (std::size_t index = 0; index < commandData_.size(); ++index) out[4 + index] = commandData_[index];
    return out;
}

Page Encoder::manufacturer() const {
    Page out = blank(page::kManufacturer);
    out[3] = 1;               // hardware revision
    put16(out, 4, 255);       // development manufacturer id
    put16(out, 6, 1);         // model number
    return out;
}

Page Encoder::product(std::uint32_t serialNumber) const {
    Page out = blank(page::kProduct);
    out[3] = 1;  // software revision
    for (int index = 0; index < 4; ++index) out[4 + index] = static_cast<std::uint8_t>(serialNumber >> (8 * index));
    return out;
}

bool Encoder::requested(std::uint8_t number, std::uint32_t serialNumber, Page& out) {
    switch (number) {
        case page::kGeneralFe: out = generalFeData(); return true;
        case page::kGeneralSettings: out = generalSettings(); return true;
        case page::kMetabolic: out = metabolic(); return true;
        case page::kCapabilities: out = capabilities(); return true;
        case page::kCommandStatus: out = commandStatus(); return true;
        case page::kManufacturer: out = manufacturer(); return true;
        case page::kProduct: out = product(serialNumber); return true;
        default:
            if (number == specificPage(profile_)) {
                out = specific();
                return true;
            }
            return false;
    }
}

Page Encoder::next(std::uint32_t serialNumber) {
    const std::uint32_t slot = counter_++;
    if (slot % 65 == 64) {
        commonToggle_ = !commonToggle_;
        return commonToggle_ ? manufacturer() : product(serialNumber);
    }
    switch ((slot - slot / 65) % 4) {
        case 1:
        case 3: return specific();
        case 2: {
            const bool settings = hasSettings();
            const bool metabolicData = hasMetabolic();
            if (settings && metabolicData) return (rotation_++ % 2) == 0 ? generalSettings() : metabolic();
            if (settings) return generalSettings();
            if (metabolicData) return metabolic();
            return generalFeData();
        }
        default: return generalFeData();
    }
}

void Encoder::recordCommand(std::uint8_t pageNumber, CommandStatus status, const Page& command) {
    commandPage_ = pageNumber;
    commandSequence_ = static_cast<std::uint8_t>(commandSequence_ + 1);
    if (commandSequence_ == 0xFF) commandSequence_ = 0;
    commandStatus_ = status;
    for (std::size_t index = 0; index < commandData_.size(); ++index) commandData_[index] = command[4 + index];
}

}  // namespace fec
}  // namespace ant
}  // namespace smartfitness
