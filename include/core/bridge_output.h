#pragma once
#include <functional>
#include "bridge/bridge_core.hpp"

class BridgeOutput {
public:
    virtual ~BridgeOutput() = default;
    virtual void begin();
    virtual void loop();
    virtual void publishTrainerState(const smartfitness::TrainerState& state);
    virtual void publishTelemetry(const FitnessTelemetry& telemetry);
    virtual void publishControlTarget(const smartfitness::ControlRequest& command);
    virtual void clearControlTarget();
    const smartfitness::TrainerState& trainerState() const;
    const FitnessTelemetry& telemetry() const;
    const smartfitness::ControlRequest& controlTarget() const;
    bool hasControlTarget() const;
    virtual void setEnabled(bool enabled);
    bool enabled() const;
    using ControlCallback = std::function<smartfitness::ControlResult(const smartfitness::ControlRequest&)>;
    void onControl(ControlCallback callback);
    smartfitness::ControlResult requestControl(const smartfitness::ControlRequest& request);

private:
    smartfitness::TrainerState trainer_;
    FitnessTelemetry telemetry_;
    smartfitness::ControlRequest target_{smartfitness::Control::Power, 0.0F};
    bool hasTarget_ = false;
    bool enabled_ = true;
    ControlCallback callback_;
};
