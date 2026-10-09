#pragma once
#include <functional>
#include "bridge/bridge_core.hpp"

class BridgeOutput {
public:
    virtual ~BridgeOutput() = default;
    virtual void begin();
    virtual void loop();
    virtual void publishTelemetry(const FitnessTelemetry& telemetry);
    virtual void publishControlTarget(const smartfitness::ControlRequest& command);
    virtual void clearControlTarget();
    const FitnessTelemetry& telemetry() const;
    const smartfitness::ControlRequest& controlTarget() const;
    bool hasControlTarget() const;
    void setEnabled(bool enabled);
    bool enabled() const;
    using ControlCallback = std::function<smartfitness::ControlResult(const smartfitness::ControlRequest&)>;
    void onControl(ControlCallback callback);
    smartfitness::ControlResult requestControl(const smartfitness::ControlRequest& request);

private:
    FitnessTelemetry telemetry_;
    smartfitness::ControlRequest target_{smartfitness::Control::Power, 0.0F};
    bool hasTarget_ = false;
    bool enabled_ = true;
    ControlCallback callback_;
};
