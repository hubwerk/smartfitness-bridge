#include "bt_device_adapter.h"
#include <cmath>
#include <utility>

using namespace smartfitness;

void BtDeviceAdapter::onTelemetry(TelemetryCallback callback) { callback_ = std::move(callback); }
void BtDeviceAdapter::emitTelemetry(const FitnessTelemetry& telemetry) {
    if (callback_) callback_(telemetry);
}

SimulatedDeviceAdapter::SimulatedDeviceAdapter(bool trainer) : trainer_(trainer) {}
void SimulatedDeviceAdapter::begin() { disconnect(); }
void SimulatedDeviceAdapter::loop() {}
bool SimulatedDeviceAdapter::isConnected() const { return telemetry_.connected; }
bool SimulatedDeviceAdapter::trainerDetected() const { return isConnected() && trainer_; }
FitnessTelemetry SimulatedDeviceAdapter::telemetry() const { return telemetry_; }

bool SimulatedDeviceAdapter::scanAndConnect() {
    telemetry_ = {};
    telemetry_.deviceName = "Simulated fitness device";
    telemetry_.deviceType = "simulation";
    telemetry_.connected = true;
    telemetry_.trainerDetected = trainer_;
    telemetry_.speed = 25;
    telemetry_.cadence = 80;
    telemetry_.power = 150;
    telemetry_.heartRate = 120;
    emitTelemetry(telemetry_);
    return true;
}

void SimulatedDeviceAdapter::disconnect() {
    telemetry_ = {};
    emitTelemetry(telemetry_);
}

void SimulatedDeviceAdapter::setTrainer(bool trainer) {
    if (trainer_ == trainer) return;
    trainer_ = trainer;
    telemetry_.trainerDetected = trainerDetected();
    telemetry_.targetPower = 0;
    emitTelemetry(telemetry_);
}

TrainerCapabilities SimulatedDeviceAdapter::capabilities() const {
    TrainerCapabilities capabilities;
    if (!isConnected()) return capabilities;
    for (Metric metric : {Metric::Speed, Metric::Cadence, Metric::Power, Metric::HeartRate, Metric::Incline})
        capabilities.setMetric(metric, true);
    if (trainer_) {
        for (Control control : {Control::Speed, Control::Incline, Control::Resistance, Control::Power})
            capabilities.setControl(control, true);
    }
    return capabilities;
}

bool SimulatedDeviceAdapter::apply(const ControlRequest& request) {
    if (!trainerDetected() || !capabilities().supports(request.control) || !std::isfinite(request.value))
        return false;
    switch (request.control) {
        case Control::Speed: telemetry_.speed = request.value; break;
        case Control::Incline: telemetry_.incline = request.value; break;
        case Control::Resistance: telemetry_.resistance = request.value; break;
        case Control::Power:
            telemetry_.power = request.value;
            telemetry_.targetPower = request.value;
            break;
        default: return false;
    }
    emitTelemetry(telemetry_);
    return true;
}
