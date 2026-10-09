#include "bridge_output.h"
#include <utility>

void BridgeOutput::begin() {
    trainer_ = {};
    telemetry_ = {};
    clearControlTarget();
}
void BridgeOutput::loop() {}
void BridgeOutput::publishTrainerState(const smartfitness::TrainerState& state) { trainer_ = state; }
void BridgeOutput::publishTelemetry(const FitnessTelemetry& telemetry) { telemetry_ = telemetry; }
void BridgeOutput::publishControlTarget(const smartfitness::ControlRequest& command) {
    target_ = command;
    hasTarget_ = true;
}
void BridgeOutput::clearControlTarget() { target_ = {smartfitness::Control::Power, 0.0F}; hasTarget_ = false; }
const smartfitness::TrainerState& BridgeOutput::trainerState() const { return trainer_; }
const FitnessTelemetry& BridgeOutput::telemetry() const { return telemetry_; }
const smartfitness::ControlRequest& BridgeOutput::controlTarget() const { return target_; }
bool BridgeOutput::hasControlTarget() const { return hasTarget_; }
void BridgeOutput::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        trainer_ = {};
        telemetry_ = {};
        clearControlTarget();
    }
}
bool BridgeOutput::enabled() const { return enabled_; }
void BridgeOutput::onControl(ControlCallback callback) { callback_ = std::move(callback); }
smartfitness::ControlResult BridgeOutput::requestControl(const smartfitness::ControlRequest& request) {
    return enabled_ && callback_ ? callback_(request) : smartfitness::ControlResult::NoAdapter;
}
