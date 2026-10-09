#include "protocols/sensor_codec.hpp"

namespace smartfitness {
namespace sensors {
namespace {

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t length) : data_(data), length_(data ? length : 0) {}
    bool ok() const { return ok_; }
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
    void skip(std::size_t width) {
        if (!ok_ || length_ - offset_ < width) {
            ok_ = false;
            return;
        }
        offset_ += width;
    }

private:
    const std::uint8_t* data_;
    std::size_t length_;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

}  // namespace

bool decodeCyclingPower(const std::uint8_t* data, std::size_t length, CyclingPowerMeasurement& out) {
    out = {};
    Reader reader(data, length);
    const std::uint32_t flags = reader.uint(2);
    out.power = static_cast<std::int16_t>(static_cast<std::uint16_t>(reader.uint(2)));
    if (flags & (1U << 0)) reader.skip(1);  // pedal power balance
    if (flags & (1U << 2)) reader.skip(2);  // accumulated torque
    if (flags & (1U << 4)) {
        out.wheel.present = true;
        out.wheel.revolutions = reader.uint(4);
        out.wheel.eventTime = static_cast<std::uint16_t>(reader.uint(2));
    }
    if (flags & (1U << 5)) {
        out.crank.present = true;
        out.crank.revolutions = reader.uint(2);
        out.crank.eventTime = static_cast<std::uint16_t>(reader.uint(2));
    }
    // Remaining optional fields (force/torque magnitudes, angles, dead spots,
    // energy) are not used and need not be validated.
    if (!reader.ok()) {
        out = {};
        return false;
    }
    return true;
}

bool decodeCsc(const std::uint8_t* data, std::size_t length, CscMeasurement& out) {
    out = {};
    Reader reader(data, length);
    const std::uint32_t flags = reader.uint(1);
    if (flags & (1U << 0)) {
        out.wheel.present = true;
        out.wheel.revolutions = reader.uint(4);
        out.wheel.eventTime = static_cast<std::uint16_t>(reader.uint(2));
    }
    if (flags & (1U << 1)) {
        out.crank.present = true;
        out.crank.revolutions = reader.uint(2);
        out.crank.eventTime = static_cast<std::uint16_t>(reader.uint(2));
    }
    if (!reader.ok()) {
        out = {};
        return false;
    }
    return true;
}

bool decodeHeartRate(const std::uint8_t* data, std::size_t length, std::uint16_t& bpm) {
    Reader reader(data, length);
    const std::uint32_t flags = reader.uint(1);
    const std::uint32_t value = (flags & 0x01U) ? reader.uint(2) : reader.uint(1);
    if (!reader.ok()) return false;
    bpm = static_cast<std::uint16_t>(value);
    return true;
}

RevolutionRate::RevolutionRate(std::uint32_t ticksPerSecond, std::uint8_t counterBits, std::uint32_t idleMs)
    : ticksPerSecond_(ticksPerSecond),
      counterMask_(counterBits >= 32 ? 0xFFFFFFFFU : ((1U << counterBits) - 1U)),
      idleMs_(idleMs) {}

float RevolutionRate::update(const RevolutionData& data, std::uint32_t nowMs) {
    if (!data.present) return current(nowMs);
    if (!hasPrevious_) {
        hasPrevious_ = true;
        previousRevolutions_ = data.revolutions;
        previousEventTime_ = data.eventTime;
        lastChangeMs_ = nowMs;
        return rpm_;
    }
    const std::uint32_t revolutions = (data.revolutions - previousRevolutions_) & counterMask_;
    const std::uint16_t ticks = static_cast<std::uint16_t>(data.eventTime - previousEventTime_);
    if (revolutions == 0 || ticks == 0) return current(nowMs);  // repeated event
    rpm_ = static_cast<float>(revolutions) * 60.0F * static_cast<float>(ticksPerSecond_) / static_cast<float>(ticks);
    previousRevolutions_ = data.revolutions;
    previousEventTime_ = data.eventTime;
    lastChangeMs_ = nowMs;
    return rpm_;
}

float RevolutionRate::current(std::uint32_t nowMs) {
    if (hasPrevious_ && nowMs - lastChangeMs_ > idleMs_) rpm_ = 0.0F;
    return rpm_;
}

void RevolutionRate::reset() {
    hasPrevious_ = false;
    previousRevolutions_ = 0;
    previousEventTime_ = 0;
    lastChangeMs_ = 0;
    rpm_ = 0.0F;
}

}  // namespace sensors
}  // namespace smartfitness
