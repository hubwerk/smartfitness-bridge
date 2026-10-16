#include "ant/sensor_codec.hpp"

#include <algorithm>
#include <cmath>

namespace smartfitness {
namespace ant {
namespace sensor {
namespace {
bool value(const TrainerState& state, Metric metric, double& out) {
    const auto& sample = state.sample(metric);
    if (!state.connected || !sample.valid ||
        !std::isfinite(sample.value) || sample.value < 0) return false;
    out = sample.value;
    return true;
}
double rate(const TrainerState& state, Metric metric, double divisor) {
    double out = 0;
    return value(state, metric, out) ? out / divisor : 0;
}
void put16(Page& page, std::size_t index, std::uint16_t n) {
    page[index] = static_cast<std::uint8_t>(n);
    page[index + 1] = static_cast<std::uint8_t>(n >> 8);
}
std::uint16_t wrap(double n, double modulus) {
    return static_cast<std::uint16_t>(std::fmod(std::floor(n), modulus));
}
}  // namespace

void Encoder::reset() { *this = Encoder{}; }

void Encoder::Events::advance(double rate, std::uint32_t elapsed, std::uint64_t now) {
    if (rate <= 0) {
        fraction = 0;
        return;
    }
    const double total = fraction + rate * elapsed / 1000.0;
    const double events = std::floor(total);
    fraction = total - events;
    if (events < 1) return;
    count = static_cast<std::uint16_t>(count + wrap(events, 65536));
    const double eventMs = static_cast<double>(now) - fraction * 1000.0 / rate;
    time = wrap(eventMs * 1.024, 65536);
}

void Encoder::update(const TrainerState& state, std::uint32_t nowMs) {
    const std::uint32_t elapsed = started_ ? nowMs - lastMs_ : 0;
    clockMs_ = started_ ? clockMs_ + elapsed : nowMs;
    const double cadence = rate(state_, Metric::Cadence, 60);
    const double speed = rate(state_, Metric::Speed, 3.6);
    crank_.advance(cadence, elapsed, clockMs_);
    wheel_.advance(speed / kWheelCircumferenceM, elapsed, clockMs_);
    beat_.advance(rate(state_, Metric::HeartRate, 60), elapsed, clockMs_);
    stride_.advance(cadence / 2, elapsed, clockMs_);
    if (state_.connected) {
        elapsedSeconds_ = std::fmod(elapsedSeconds_ + elapsed / 1000.0, 256.0);
        distanceM_ = std::fmod(distanceM_ + speed * elapsed / 1000.0, 256.0);
    }
    state_ = state;
    lastMs_ = nowMs;
    started_ = true;
}

Page Encoder::bicyclePower() {
    double watts = 0, cadence = 0;
    const bool valid = value(state_, Metric::Power, watts);
    const auto power = valid ? static_cast<std::uint16_t>(std::min(65534.0, std::round(watts))) : 0xFFFF;
    if (valid) {
        ++powerEvents_;
        accumulatedPower_ = static_cast<std::uint16_t>(accumulatedPower_ + power);
    }
    Page page{16, powerEvents_, 0xFF, 0xFF, 0, 0, 0, 0};
    if (value(state_, Metric::Cadence, cadence)) page[3] = static_cast<std::uint8_t>(std::min(254.0, std::round(cadence)));
    put16(page, 4, accumulatedPower_);
    put16(page, 6, power);
    return page;
}

Page Encoder::heartRate() {
    double bpm = 0;
    // The page-change toggle changes every four messages, including page zero.
    Page page{static_cast<std::uint8_t>(((heartPages_++ / 4) & 1) << 7), 0xFF, 0xFF, 0xFF, 0, 0,
              static_cast<std::uint8_t>(beat_.count), 0};
    put16(page, 4, beat_.time);
    if (value(state_, Metric::HeartRate, bpm)) page[7] = static_cast<std::uint8_t>(std::min(254.0, std::round(bpm)));
    return page;
}

Page Encoder::speedCadence() const {
    Page page{};
    put16(page, 0, crank_.time);
    put16(page, 2, crank_.count);
    put16(page, 4, wheel_.time);
    put16(page, 6, wheel_.count);
    return page;
}

Page Encoder::stride() const {
    double speed = 0;
    const bool validSpeed = value(state_, Metric::Speed, speed);
    speed = validSpeed ? speed / 3.6 : 0;
    const auto speedRaw = static_cast<std::uint16_t>(std::min(4095.0, std::round(speed * 256)));
    const auto distanceRaw = wrap(distanceM_ * 16, 4096);
    Page page{1, static_cast<std::uint8_t>(std::fmod(elapsedSeconds_, 1.0) * 200),
              static_cast<std::uint8_t>(elapsedSeconds_), static_cast<std::uint8_t>(distanceRaw >> 4),
              static_cast<std::uint8_t>(((distanceRaw & 15) << 4) | (speedRaw >> 8)),
              static_cast<std::uint8_t>(speedRaw), static_cast<std::uint8_t>(stride_.count), 0xFF};
    // Latency is unavailable: source samples do not expose their capture time.
    return page;
}

}  // namespace sensor
}  // namespace ant
}  // namespace smartfitness
