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
    telemetry_.speedValid = true;
    telemetry_.cadenceValid = true;
    telemetry_.powerValid = true;
    telemetry_.heartRateValid = true;
    telemetry_.metrics.speed = {telemetry_.speed, true};
    telemetry_.metrics.cadence = {telemetry_.cadence, true};
    telemetry_.metrics.power = {telemetry_.power, true};
    telemetry_.metrics.heartRate = {telemetry_.heartRate, true};
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
    telemetry_.targetPowerValid = false;
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
        case Control::Speed:
            telemetry_.speed = request.value;
            telemetry_.speedValid = true;
            telemetry_.metrics.speed = {request.value, true};
            break;
        case Control::Incline:
            telemetry_.incline = request.value;
            telemetry_.inclineValid = true;
            telemetry_.metrics.incline = {request.value, true};
            break;
        case Control::Resistance:
            telemetry_.resistance = request.value;
            telemetry_.resistanceValid = true;
            telemetry_.metrics.resistance = {request.value, true};
            break;
        case Control::Power:
            telemetry_.power = request.value;
            telemetry_.targetPower = request.value;
            telemetry_.powerValid = true;
            telemetry_.targetPowerValid = true;
            telemetry_.metrics.power = {request.value, true};
            break;
        default: return false;
    }
    emitTelemetry(telemetry_);
    return true;
}
