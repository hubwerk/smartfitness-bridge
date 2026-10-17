#include "bridge/bridge_core.hpp"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"

#include <cmath>
#ifdef ARDUINO
#include <Arduino.h>
#include "debug_serial.h"
#endif

namespace smartfitness {
namespace {
template <typename T>
std::uint32_t enumBit(T value) {
    const auto index = static_cast<unsigned int>(value);
    return index < static_cast<unsigned int>(T::Count) ? (1U << index) : 0U;
}
}

void TrainerCapabilities::setMetric(Metric metric, bool supported) {
    if (supported) metrics |= enumBit(metric);
    else metrics &= ~enumBit(metric);
}

void TrainerCapabilities::setControl(Control control, bool supported) {
    if (supported) controls |= enumBit(control);
    else controls &= ~enumBit(control);
}

bool TrainerCapabilities::supports(Metric metric) const { return (metrics & enumBit(metric)) != 0; }
bool TrainerCapabilities::supports(Control control) const { return (controls & enumBit(control)) != 0; }

const MetricSample& TrainerState::sample(Metric metric) const {
    static const MetricSample invalid;
    const auto index = static_cast<std::size_t>(metric);
    return index < telemetry.size() ? telemetry[index] : invalid;
}

void TrainerState::setSample(Metric metric, float value, bool valid) {
    const auto index = static_cast<std::size_t>(metric);
    if (index < telemetry.size()) telemetry[index] = {value, valid && std::isfinite(value)};
}

BridgeCore::BridgeCore(TrainerControlAdapter* adapter) : adapter_(adapter) {}

BridgeCore::~BridgeCore() {
    if (device_) device_->onTelemetry({});
    if (ant_) ant_->onControl({});
    if (ftms_) ftms_->onControl({});
}

void BridgeCore::setAdapter(TrainerControlAdapter* adapter) {
    adapter_ = adapter;
    target_ = {Control::Power, 0.0F};
    hasTarget_ = false;
    dirty_ = true;
}

void BridgeCore::attach(BtDeviceAdapter& adapter, AntOutput& ant, FtmsOutput& ftms) {
    if (device_) device_->onTelemetry({});
    if (ant_) ant_->onControl({});
    if (ftms_) ftms_->onControl({});
    device_ = &adapter;
    ant_ = &ant;
    ftms_ = &ftms;
    setAdapter(&adapter);
    trainer_ = {};
    telemetry_ = {};
    device_->onTelemetry([this](const FitnessTelemetry& value) { setTelemetry(value); });
    ant_->onControl([this](const ControlRequest& request) { return routeControl(request); });
    ftms_->onControl([this](const ControlRequest& request) { return routeControl(request); });
}

void BridgeCore::begin() {
    if (device_) device_->begin();
    if (ant_) ant_->begin();
    if (ftms_) ftms_->begin();
    refreshDeviceState();
    publish();
}

void BridgeCore::loop() {
    if (device_) device_->loop();
    refreshDeviceState();
    const bool antEnabled = ant_ && ant_->enabled();
    const bool ftmsEnabled = ftms_ && ftms_->enabled();
    if (antEnabled != antEnabled_ || ftmsEnabled != ftmsEnabled_) dirty_ = true;
    antEnabled_ = antEnabled;
    ftmsEnabled_ = ftmsEnabled;
    if (dirty_) publish();
    if (ant_ && ant_->enabled()) ant_->loop();
    if (ftms_ && ftms_->enabled()) ftms_->loop();
}

bool BridgeCore::scanAndConnect() {
    if (!device_) return false;
    disconnect();
    const bool connected = device_->scanAndConnect();
    setTelemetry(device_->telemetry());
    refreshDeviceState();
    return connected && trainer_.connected;
}

void BridgeCore::disconnect() {
    if (device_) device_->disconnect();
    setTrainerState({});
    telemetry_ = {};
    dirty_ = true;
}

void BridgeCore::refreshDeviceState() {
    if (!device_) return;
    TrainerState next = trainer_;
    next.connected = device_->isConnected();
    next.trainerDetected = next.connected && device_->trainerDetected();
    next.capabilities = next.connected ? device_->capabilities() : TrainerCapabilities{};
    next.profile = device_->profile();
    setTrainerState(next);
}

void BridgeCore::setTrainerState(const TrainerState& state) {
#ifdef ARDUINO
    if (trainer_.connected != state.connected)
        DBG_SERIAL.printf("[BridgeCore] BLE input %s\n", state.connected ? "connected" : "disconnected");
    if (trainer_.connected && state.connected && trainer_.profile != state.profile)
        DBG_SERIAL.printf("[BridgeCore] input profile: %s\n", machineProfileName(state.profile));
#endif
    if (trainer_.connected != state.connected ||
        trainer_.trainerDetected != state.trainerDetected ||
        trainer_.capabilities.controls != state.capabilities.controls ||
        trainer_.capabilities.metrics != state.capabilities.metrics ||
        trainer_.profile != state.profile) {
        hasTarget_ = false;
        target_ = {Control::Power, 0.0F};
        dirty_ = true;
    }
    trainer_ = state;
    for (std::size_t index = 0; index < trainer_.telemetry.size(); ++index) {
        if (!trainer_.capabilities.supports(static_cast<Metric>(index)))
            trainer_.telemetry[index].valid = false;
    }
    if (!trainer_.connected) {
        trainer_ = {};
        trainer_.profile = state.profile;
        telemetry_ = {};
        hasTarget_ = false;
        target_ = {Control::Power, 0.0F};
    }
    telemetry_.connected = trainer_.connected;
    telemetry_.trainerDetected = trainer_.trainerDetected;
}

const TrainerState& BridgeCore::trainerState() const { return trainer_; }
const FitnessTelemetry& BridgeCore::telemetry() const { return telemetry_; }
const ControlRequest& BridgeCore::controlTarget() const { return target_; }
bool BridgeCore::hasControlTarget() const { return hasTarget_; }

void BridgeCore::setTelemetry(const FitnessTelemetry& telemetry) {
    telemetry_ = telemetry;
    if (device_) refreshDeviceState();
    else {
        TrainerState next = trainer_;
        next.connected = telemetry.connected;
        next.trainerDetected = telemetry.trainerDetected;
        setTrainerState(next);
    }
    // Only values the device actually sent become valid samples.
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        const auto metric = static_cast<Metric>(index);
        const auto& value = telemetry_.metric(metric);
        trainer_.setSample(metric, value.value_or(0.0F),
                           trainer_.connected && value.has_value() && trainer_.capabilities.supports(metric));
    }
    dirty_ = true;
}

RuntimeState BridgeCore::state() const {
    if (!trainer_.connected) return RuntimeState::Disconnected;
    if (!trainer_.trainerDetected) return RuntimeState::Connected;
    return canControl() ? RuntimeState::ControlReady : RuntimeState::Trainer;
}

bool BridgeCore::canControl() const {
    return trainer_.connected && trainer_.trainerDetected && adapter_ &&
        (trainer_.capabilities.controls & ((1U << static_cast<unsigned int>(Control::Count)) - 1U));
}

bool BridgeCore::isValidValue(const ControlRequest& request) {
    if (!std::isfinite(request.value)) return false;
    const auto within = [&request](const ControlBounds& bounds) {
        return request.value >= bounds.minimum && request.value <= bounds.maximum;
    };
    switch (request.control) {
        case Control::Speed: return within(kSpeedBounds);
        case Control::Incline: return within(kInclineBounds);
        case Control::Resistance: return within(kResistanceBounds);
        case Control::Power: return within(kPowerBounds);
        default: return false;
    }
}

ControlResult BridgeCore::routeControl(const ControlRequest& request) {
    refreshDeviceState();
    if (!trainer_.connected) return ControlResult::Disconnected;
    if (!trainer_.trainerDetected) return ControlResult::NotTrainer;
    if (!trainer_.capabilities.supports(request.control)) return ControlResult::Unsupported;
    if (!isValidValue(request)) return ControlResult::InvalidValue;
    if (!adapter_) return ControlResult::NoAdapter;
    if (!adapter_->apply(request)) return ControlResult::AdapterRejected;
    refreshDeviceState();
    if (!canControl() || !trainer_.capabilities.supports(request.control))
        return ControlResult::AdapterRejected;
    target_ = request;
    hasTarget_ = true;
    dirty_ = true;
    return ControlResult::Applied;
}

void BridgeCore::publish() {
    for (BridgeOutput* output : {static_cast<BridgeOutput*>(ant_), static_cast<BridgeOutput*>(ftms_)}) {
        if (!output) continue;
        if (!hasTarget_) output->clearControlTarget();
        if (!output->enabled()) continue;
        output->publishTrainerState(trainer_);
        output->publishTelemetry(telemetry_);
        if (hasTarget_) output->publishControlTarget(target_);
    }
    dirty_ = false;
}

}  // namespace smartfitness
