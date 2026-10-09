#include "bridge/bridge_core.hpp"

#include <cmath>
#include <cstddef>

namespace smartfitness {
namespace {

constexpr std::uint32_t bitFor(std::uint8_t value) {
    return std::uint32_t{1} << value;
}

}  // namespace

void TrainerCapabilities::setMetric(Metric metric, bool supported) {
    const auto bit = bitFor(static_cast<std::uint8_t>(metric));
    metrics = supported ? metrics | bit : metrics & ~bit;
}

void TrainerCapabilities::setControl(Control control, bool supported) {
    const auto bit = bitFor(static_cast<std::uint8_t>(control));
    controls = supported ? controls | bit : controls & ~bit;
}

bool TrainerCapabilities::supports(Metric metric) const {
    return (metrics & bitFor(static_cast<std::uint8_t>(metric))) != 0;
}

bool TrainerCapabilities::supports(Control control) const {
    return (controls & bitFor(static_cast<std::uint8_t>(control))) != 0;
}

const MetricSample& TrainerState::sample(Metric metric) const {
    return telemetry[static_cast<std::size_t>(metric)];
}

void TrainerState::setSample(Metric metric, float value, bool valid) {
    telemetry[static_cast<std::size_t>(metric)] = {value, valid};
}

BridgeCore::BridgeCore(TrainerControlAdapter* adapter) : adapter_(adapter) {}

void BridgeCore::setAdapter(TrainerControlAdapter* adapter) {
    adapter_ = adapter;
}

void BridgeCore::setTrainerState(const TrainerState& state) {
    trainer_ = state;
}

const TrainerState& BridgeCore::trainerState() const {
    return trainer_;
}

ControlResult BridgeCore::routeControl(const ControlRequest& request) const {
    if (!trainer_.connected) {
        return ControlResult::Disconnected;
    }
    if (!trainer_.capabilities.supports(request.control)) {
        return ControlResult::Unsupported;
    }
    if (!isValidValue(request)) {
        return ControlResult::InvalidValue;
    }
    if (adapter_ == nullptr) {
        return ControlResult::NoAdapter;
    }
    return adapter_->apply(request) ? ControlResult::Applied : ControlResult::AdapterRejected;
}

bool BridgeCore::isValidValue(const ControlRequest& request) {
    if (!std::isfinite(request.value)) {
        return false;
    }

    switch (request.control) {
    case Control::Speed:
        return request.value >= 0.0F && request.value <= 20.0F;
    case Control::Incline:
        return request.value >= -10.0F && request.value <= 25.0F;
    case Control::Resistance:
        return request.value >= 0.0F && request.value <= 100.0F;
    case Control::Count:
        return false;
    }
    return false;
}

}  // namespace smartfitness
