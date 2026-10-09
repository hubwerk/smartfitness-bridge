#include "bridge_core.h"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"

#include <cmath>

namespace smartfitness {
namespace {
template <typename T>
std::uint32_t bit(T value) {
    const auto index = static_cast<unsigned int>(value);
    return index < static_cast<unsigned int>(T::Count) ? (1U << index) : 0U;
}
}

void TrainerCapabilities::setMetric(Metric metric, bool supported) {
    if (supported) metrics |= bit(metric);
    else metrics &= ~bit(metric);
}

void TrainerCapabilities::setControl(Control control, bool supported) {
    if (supported) controls |= bit(control);
    else controls &= ~bit(control);
}

bool TrainerCapabilities::supports(Metric metric) const { return (metrics & bit(metric)) != 0; }
bool TrainerCapabilities::supports(Control control) const { return (controls & bit(control)) != 0; }

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
    setTrainerState(next);
}

void BridgeCore::setTrainerState(const TrainerState& state) {
    if (trainer_.connected != state.connected ||
        trainer_.trainerDetected != state.trainerDetected ||
        trainer_.capabilities.controls != state.capabilities.controls ||
        trainer_.capabilities.metrics != state.capabilities.metrics) {
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
    trainer_.setSample(Metric::Speed, telemetry_.speed, trainer_.connected && trainer_.capabilities.supports(Metric::Speed));
    trainer_.setSample(Metric::Cadence, telemetry_.cadence, trainer_.connected && trainer_.capabilities.supports(Metric::Cadence));
    trainer_.setSample(Metric::Power, telemetry_.power, trainer_.connected && trainer_.capabilities.supports(Metric::Power));
    trainer_.setSample(Metric::HeartRate, telemetry_.heartRate, trainer_.connected && trainer_.capabilities.supports(Metric::HeartRate));
    trainer_.setSample(Metric::Incline, telemetry_.incline, trainer_.connected && trainer_.capabilities.supports(Metric::Incline));
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
    switch (request.control) {
        case Control::Speed: return request.value >= 0 && request.value <= 100;
        case Control::Incline: return request.value >= -20 && request.value <= 25;
        case Control::Resistance: return request.value >= 0 && request.value <= 100;
        case Control::Power: return request.value >= 0 && request.value <= 2000;
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
        output->publishTelemetry(telemetry_);
        if (hasTarget_) output->publishControlTarget(target_);
    }
    dirty_ = false;
}

}  // namespace smartfitness
