#include "ftms_output.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#ifdef ARDUINO
#include <NimBLEDevice.h>
#endif

using namespace smartfitness;

namespace {
constexpr std::uint32_t kAdvertiseRetryMs = 1000;
constexpr float kRadiansToDegrees = 57.2957795131F;
}

#ifdef ARDUINO
// NimBLE callbacks run on the BLE host task: they only queue events for loop().
struct FtmsOutput::Transport : public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
    explicit Transport(FtmsOutput& output) : owner(output) {}

    using NimBLEServerCallbacks::onConnect;
    using NimBLEServerCallbacks::onDisconnect;
    using NimBLECharacteristicCallbacks::onWrite;

    void onConnect(NimBLEServer*) override { owner.notifyClientConnected(); }
    void onConnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
        // NimBLE stores addresses little endian; the bridge uses display order.
        std::array<std::uint8_t, 6> address{};
        for (std::size_t index = 0; index < address.size(); ++index) address[index] = desc->peer_id_addr.val[5 - index];
        owner.setClientAddress(address, desc->peer_id_addr.type);
    }
    void onDisconnect(NimBLEServer*) override {
        owner.clearClientAddress();
        owner.notifyClientDisconnected();
    }
    void onWrite(NimBLECharacteristic* characteristic) override {
        const NimBLEAttValue value = characteristic->getValue();
        owner.enqueueControlPoint(value.data(), value.length());
    }

    FtmsOutput& owner;
    NimBLEServer* server = nullptr;
    NimBLECharacteristic* feature = nullptr;
    std::array<NimBLECharacteristic*, 6> machineData{};
    std::array<NimBLECharacteristic*, 4> ranges{};
    NimBLECharacteristic* trainingStatus = nullptr;
    NimBLECharacteristic* controlPoint = nullptr;
    NimBLECharacteristic* machineStatus = nullptr;
};
#else
struct FtmsOutput::Transport {};
#endif

FtmsOutput::FtmsOutput() = default;

FtmsOutput::~FtmsOutput() {
#ifdef ARDUINO
    if (transport_) NimBLEDevice::deinit(true);
#endif
}

void FtmsOutput::begin() {
    BridgeOutput::begin();
    begun_ = true;
    resetSession();
    refreshProfile();
    feature_ = ftms::encodeFeature(trainerState().capabilities, profile_, outputMetrics());
    transportSetFeature();
}

void FtmsOutput::loop() {
    handleConnectionChanges();
    drainControlPoint();
    updateTrainingStatus();
    publishMachineData();
}

void FtmsOutput::setEnabled(bool enabled) {
    BridgeOutput::setEnabled(enabled);
    if (enabled || !begun_) return;
    transportDisconnectClients();
    stopAdvertising();
    resetSession();
    connected_ = false;
}

void FtmsOutput::publishTrainerState(const TrainerState& state) {
    const bool inputProfileChanged = trainerState().profile != state.profile;
    const bool lostTreadmill = profile_ == MachineProfile::Treadmill && trainerState().connected &&
        (!state.connected || (trainerState().trainerDetected && !state.trainerDetected));
    BridgeOutput::publishTrainerState(state);
    refreshProfile();
    if (lostTreadmill || inputProfileChanged) {
        generation_.fetch_add(1);
        resetSession();
    }
    auto feature = ftms::encodeFeature(state.capabilities, profile_, outputMetrics());
    if (feature == feature_) return;
    feature_ = std::move(feature);
    transportSetFeature();
}

void FtmsOutput::setProfileOverride(FtmsOutputProfile profile) {
    if (profile == profileOverride_) return;
    generation_.fetch_add(1);
    resetSession();
    profileOverride_ = profile;
    refreshProfile();
    feature_ = ftms::encodeFeature(trainerState().capabilities, profile_, outputMetrics());
    transportSetFeature();
}

MachineProfile FtmsOutput::profile() const { return profile_; }

void FtmsOutput::refreshProfile() {
    const auto next = effectiveOutputProfile(profileOverride_, trainerState().profile);
    if (next == profile_) return;
    profile_ = next;
    feature_ = ftms::encodeFeature(trainerState().capabilities, profile_, outputMetrics());
    transportSetFeature();
    generation_.fetch_add(1);
    resetSession();
    response_.clear();
    status_.clear();
    bikeData_.clear();
    treadmillData_.clear();
    crossTrainerData_.clear();
    rowerData_.clear();
    stepClimberData_.clear();
    stairClimberData_.clear();
    machineDataPackets_.clear();
    trainingStatus_ = ftms::TrainingStatus::Idle;
    profileDisconnectPending_ = connections_.load() > 0;
    transportDisconnectClients();
    connected_ = false;
    stopAdvertising();
    transportApplyProfile();
    startAdvertising();
}

void FtmsOutput::setDeviceName(const std::string& name) {
    std::string next = name.substr(0, kMaxNameLength);
    if (next.empty()) next = kDefaultName;
    if (next == name_) return;
    name_ = next;
    transportApplyName();
}

const std::string& FtmsOutput::deviceName() const { return name_; }

bool FtmsOutput::enqueueControlPoint(const std::uint8_t* data, std::size_t length) {
    if (!data || length == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (queueCount_ == queue_.size()) return false;
    PendingWrite& write = queue_[(queueHead_ + queueCount_) % queue_.size()];
    write.truncated = length > write.data.size();
    write.length = static_cast<std::uint8_t>(write.truncated ? write.data.size() : length);
    for (std::size_t index = 0; index < write.length; ++index) write.data[index] = data[index];
    write.generation = generation_.load();
    ++queueCount_;
    return true;
}

void FtmsOutput::notifyClientConnected() {
    connections_.fetch_add(1);
    generation_.fetch_add(1);
}

void FtmsOutput::notifyClientDisconnected() {
    int current = connections_.load();
    while (current > 0 && !connections_.compare_exchange_weak(current, current - 1)) {}
    generation_.fetch_add(1);
}

void FtmsOutput::setClientAddress(const std::array<std::uint8_t, 6>& address, std::uint8_t type) {
    std::lock_guard<std::mutex> lock(mutex_);
    clientAddress_ = address;
    clientAddressType_ = type;
    clientAddressValid_ = true;
}

void FtmsOutput::clearClientAddress() {
    std::lock_guard<std::mutex> lock(mutex_);
    clientAddressValid_ = false;
}

bool FtmsOutput::clientAddress(std::array<std::uint8_t, 6>& address, std::uint8_t& type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clientAddressValid_) return false;
    address = clientAddress_;
    type = clientAddressType_;
    return true;
}

bool FtmsOutput::advertising() const { return advertising_; }
bool FtmsOutput::clientConnected() const { return connected_; }
bool FtmsOutput::controlGranted() const { return granted_; }
const ftms::Bytes& FtmsOutput::lastIndoorBikeData() const { return bikeData_; }
const ftms::Bytes& FtmsOutput::lastTreadmillData() const { return treadmillData_; }
const ftms::Bytes& FtmsOutput::lastCrossTrainerData() const { return crossTrainerData_; }
const ftms::Bytes& FtmsOutput::lastRowerData() const { return rowerData_; }
const ftms::Bytes& FtmsOutput::lastStepClimberData() const { return stepClimberData_; }
const ftms::Bytes& FtmsOutput::lastStairClimberData() const { return stairClimberData_; }
ftms::Bytes& FtmsOutput::machineData() {
    switch (profile_) {
        case MachineProfile::Treadmill: return treadmillData_;
        case MachineProfile::CrossTrainer: return crossTrainerData_;
        case MachineProfile::Rower: return rowerData_;
        case MachineProfile::StepClimber: return stepClimberData_;
        case MachineProfile::StairClimber: return stairClimberData_;
        default: return bikeData_;
    }
}
const ftms::Bytes& FtmsOutput::lastMachineData() const {
    switch (profile_) {
        case MachineProfile::Treadmill: return treadmillData_;
        case MachineProfile::CrossTrainer: return crossTrainerData_;
        case MachineProfile::Rower: return rowerData_;
        case MachineProfile::StepClimber: return stepClimberData_;
        case MachineProfile::StairClimber: return stairClimberData_;
        default: return bikeData_;
    }
}
const std::vector<ftms::Bytes>& FtmsOutput::lastMachineDataPackets() const { return machineDataPackets_; }
const ftms::Bytes& FtmsOutput::lastTrainingStatus() const { return trainingData_; }
const ftms::Bytes& FtmsOutput::lastControlResponse() const { return response_; }
const ftms::Bytes& FtmsOutput::lastMachineStatus() const { return status_; }
const ftms::Bytes& FtmsOutput::feature() const { return feature_; }
ftms::Bytes FtmsOutput::speedRange() const {
    ftms::Features features;
    return ftms::decodeFeature(feature_.data(), feature_.size(), features) &&
        (features.targets & ftms::kTargetSpeed) ? ftms::encodeSupportedSpeedRange() : ftms::Bytes{};
}
void FtmsOutput::setClock(Clock clock) { clock_ = std::move(clock); }

std::uint32_t FtmsOutput::now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return static_cast<std::uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void FtmsOutput::handleConnectionChanges() {
    if (connections_.load() == 0) profileDisconnectPending_ = false;
    connected_ = enabled() && !profileDisconnectPending_ && connections_.load() > 0;
    const std::uint32_t generation = generation_.load();
    if (generation != seenGeneration_) {
        // Every connect or disconnect starts a new session: control must be requested again.
        seenGeneration_ = generation;
        granted_ = false;
        bikeDataSent_ = false;
        trainingStatusSent_ = false;
        sessionStopped_ = false;
        treadmillData_.clear();
        machineDataPackets_.clear();
        // The stack stops advertising once a central connects.
        if (connected_) advertising_ = false;
    }
    if (!connected_) startAdvertising();
}

void FtmsOutput::drainControlPoint() {
    std::array<PendingWrite, kQueueCapacity> pending;
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (; count < queueCount_; ++count) pending[count] = queue_[(queueHead_ + count) % queue_.size()];
        queueHead_ = 0;
        queueCount_ = 0;
    }
    for (std::size_t index = 0; index < count; ++index) {
        // Writes from an earlier (now disconnected) session are dropped.
        if (connected_ && pending[index].generation == seenGeneration_ &&
            pending[index].generation == generation_.load() && connections_.load() > 0)
            handleControlPoint(pending[index]);
    }
}

void FtmsOutput::handleControlPoint(const PendingWrite& write) {
    ftms::ControlPointCommand command;
    if (!ftms::decodeControlPoint(write.data.data(), write.length, command, profile_)) return;
    ftms::ResultCode result = write.truncated ? ftms::ResultCode::InvalidParameter : command.status;
    if (result == ftms::ResultCode::Success) {
        if (command.opcode == static_cast<std::uint8_t>(ftms::OpCode::RequestControl)) {
            granted_ = true;
        } else if (!granted_) {
            result = ftms::ResultCode::ControlNotPermitted;
        } else if (profile_ == MachineProfile::Treadmill &&
                   command.opcode == static_cast<std::uint8_t>(ftms::OpCode::StopOrPause)) {
            result = requestControl({Control::Speed, 0.0F}) == ControlResult::Applied
                ? ftms::ResultCode::Success : ftms::ResultCode::OperationFailed;
        } else if (profile_ == MachineProfile::Treadmill &&
                   command.opcode == static_cast<std::uint8_t>(ftms::OpCode::StartOrResume)) {
            // No start primitive exists in the core: never invent a target speed.
            result = ftms::ResultCode::OperationFailed;
        } else if (command.hasRequest) {
            // Always route through the core gate; never apply directly.
            result = ftms::resultCodeFor(requestControl(command.request));
        } else if (command.opcode == static_cast<std::uint8_t>(ftms::OpCode::Reset)) {
            granted_ = false;
        }
    }
    response_ = ftms::encodeControlPointResponse(command.opcode, result);
    transportIndicateResponse();
    if (result != ftms::ResultCode::Success) return;
    const auto opcode = static_cast<ftms::OpCode>(command.opcode);
    if (opcode == ftms::OpCode::Reset || opcode == ftms::OpCode::StopOrPause ||
        opcode == ftms::OpCode::StartOrResume) {
        sessionStopped_ = opcode != ftms::OpCode::StartOrResume;
        trainingStatus_ = sessionStopped_ ? ftms::TrainingStatus::Idle : ftms::TrainingStatus::ManualMode;
        transportNotifyTrainingStatus();
        trainingStatusSent_ = true;
        if (opcode == ftms::OpCode::Reset) bikeDataSent_ = false;
    } else if (opcode == ftms::OpCode::SetTargetSpeed && command.request.value > 0) {
        sessionStopped_ = false;
    }
    auto status = ftms::encodeMachineStatus(command);
    if (status.empty()) return;
    status_ = std::move(status);
    transportNotifyStatus();
}

FitnessMetrics FtmsOutput::outputMetrics() const {
    const auto& state = trainerState();
    if (!state.connected) return {};
    auto metrics = state.metrics;
    const auto legacy = [&state](MetricSample& field, Metric metric) {
        if (!field.valid) field = state.sample(metric);
        if (!std::isfinite(field.value)) field.valid = false;
    };
    legacy(metrics.speed, Metric::Speed);
    legacy(metrics.distance, Metric::Distance);
    legacy(metrics.cadence, Metric::Cadence);
    legacy(metrics.power, Metric::Power);
    legacy(metrics.heartRate, Metric::HeartRate);
    legacy(metrics.incline, Metric::Incline);
    legacy(metrics.elapsed, Metric::ElapsedTime);
    for (const auto field : kFitnessMetricFields)
        if (!std::isfinite((metrics.*field).value)) (metrics.*field).valid = false;
    auto mapped = ftms::mapMachineData(metrics, state.profile, profile_);
    if ((profile_ == MachineProfile::Treadmill || profile_ == MachineProfile::CrossTrainer) &&
        mapped.incline.valid && std::isfinite(mapped.incline.value) &&
        (!mapped.rampAngle.valid || !std::isfinite(mapped.rampAngle.value))) {
        mapped.rampAngle = {std::atan(mapped.incline.value / 100.0F) * kRadiansToDegrees, true};
    }
    return mapped;
}

void FtmsOutput::publishMachineData() {
    if (!connected_) return;
    const auto metrics = outputMetrics();
    auto payload = ftms::encodeMachineData(profile_, metrics);
    auto packets = ftms::splitMachineData(profile_, metrics, transportMaxPayload());
    auto& data = machineData();
    const std::uint32_t timestamp = now();
    const std::uint32_t elapsed = timestamp - lastBikeDataMs_;
    if (bikeDataSent_) {
        if (elapsed < kBikeDataMinIntervalMs) return;
        if (payload == data && packets == machineDataPackets_ && elapsed < kBikeDataMaxIntervalMs) return;
    }
    data = std::move(payload);
    machineDataPackets_ = std::move(packets);
    lastBikeDataMs_ = timestamp;
    bikeDataSent_ = true;
    transportNotifyMachineData();
}

void FtmsOutput::updateTrainingStatus() {
    auto status = trainerState().connected && !sessionStopped_
        ? ftms::TrainingStatus::ManualMode : ftms::TrainingStatus::Idle;
    if (profile_ == MachineProfile::Treadmill) {
        const auto speed = outputMetrics().speed;
        status = !sessionStopped_ && trainerState().connected && speed.valid &&
            std::isfinite(speed.value) && speed.value > 0
            ? ftms::TrainingStatus::ManualMode : ftms::TrainingStatus::Idle;
    }
    if (status == trainingStatus_ && (!connected_ || trainingStatusSent_)) return;
    trainingStatus_ = status;
    transportNotifyTrainingStatus();
    trainingStatusSent_ = connected_;
    if (profile_ == MachineProfile::Treadmill && connected_) {
        status_ = status == ftms::TrainingStatus::Idle ? ftms::Bytes{0x02, 0x01} : ftms::Bytes{0x04};
        transportNotifyStatus();
        trainingStatusSent_ = true;
    }
}

void FtmsOutput::startAdvertising() {
    if (!begun_ || !enabled() || connected_ || advertising_ || profileDisconnectPending_) return;
    const std::uint32_t timestamp = now();
    if (advertiseFailed_ && timestamp - advertiseFailedMs_ < kAdvertiseRetryMs) return;
    advertising_ = transportBegin() && transportStartAdvertising();
    advertiseFailed_ = !advertising_;
    advertiseFailedMs_ = timestamp;
}

void FtmsOutput::stopAdvertising() {
    if (advertising_) transportStopAdvertising();
    advertising_ = false;
    advertiseFailed_ = false;
}

void FtmsOutput::resetSession() {
    granted_ = false;
    bikeDataSent_ = false;
    trainingStatusSent_ = false;
    sessionStopped_ = false;
    treadmillData_.clear();
    machineDataPackets_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    queueHead_ = 0;
    queueCount_ = 0;
}

#ifdef ARDUINO

bool FtmsOutput::transportBegin() {
    if (transport_) return true;
    if (!transport_) {
        transport_.reset(new Transport(*this));
        // The central and peripheral share one stack; never deinitialize it to
        // rebuild the peripheral's GATT service.
        if (!NimBLEDevice::getInitialized()) NimBLEDevice::init(name_);
        NimBLEDevice::setDeviceName(name_);
        transport_->server = NimBLEDevice::createServer();
        transport_->server->setCallbacks(transport_.get(), false);
        transport_->server->advertiseOnDisconnect(false);
    }
    auto* transport = transport_.get();
    NimBLEService* service = transport->server->createService(NimBLEUUID(ftms::kServiceUuid));
    transport->feature = service->createCharacteristic(NimBLEUUID(ftms::kFeatureUuid), NIMBLE_PROPERTY::READ);
    const std::array<std::uint16_t, 6> dataUuids{ftms::kIndoorBikeDataUuid, ftms::kTreadmillDataUuid,
        ftms::kCrossTrainerDataUuid, ftms::kRowerDataUuid, ftms::kStepClimberDataUuid, ftms::kStairClimberDataUuid};
    for (std::size_t index = 0; index < dataUuids.size(); ++index)
        transport->machineData[index] = service->createCharacteristic(NimBLEUUID(dataUuids[index]), NIMBLE_PROPERTY::NOTIFY);
    transport->trainingStatus = service->createCharacteristic(
        NimBLEUUID(ftms::kTrainingStatusUuid), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    const std::array<std::uint16_t, 4> rangeUuids{ftms::kSupportedSpeedRangeUuid,
        ftms::kSupportedInclinationRangeUuid, ftms::kSupportedResistanceRangeUuid, ftms::kSupportedPowerRangeUuid};
    for (std::size_t index = 0; index < rangeUuids.size(); ++index)
        transport->ranges[index] = service->createCharacteristic(NimBLEUUID(rangeUuids[index]), NIMBLE_PROPERTY::READ);
    transport->controlPoint = service->createCharacteristic(
        NimBLEUUID(ftms::kControlPointUuid), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
    transport->machineStatus = service->createCharacteristic(NimBLEUUID(ftms::kMachineStatusUuid), NIMBLE_PROPERTY::NOTIFY);
    transport->controlPoint->setCallbacks(transport);

    transport->feature->setValue(feature_);
    const auto training = ftms::encodeTrainingStatus(trainingStatus_);
    transport->trainingStatus->setValue(training);
    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(NimBLEUUID(ftms::kServiceUuid));
    const auto serviceData = ftms::encodeAdvertisingServiceData(profile_);
    advertising->setServiceData(NimBLEUUID(ftms::kServiceUuid), std::string(serviceData.begin(), serviceData.end()));
    advertising->setScanResponse(true);
    advertising->setName(name_);
    transportApplyProfile();
    return true;
}

bool FtmsOutput::transportStartAdvertising() { return NimBLEDevice::getAdvertising()->start(); }

void FtmsOutput::transportStopAdvertising() {
    if (transport_) NimBLEDevice::getAdvertising()->stop();
}

void FtmsOutput::transportDisconnectClients() {
    if (!transport_) return;
    for (const std::uint16_t id : transport_->server->getPeerDevices()) transport_->server->disconnect(id);
}

void FtmsOutput::transportApplyName() {
    if (!transport_) return;
    NimBLEDevice::setDeviceName(name_);
    NimBLEDevice::getAdvertising()->setName(name_);
    if (!advertising_) return;
    transportStopAdvertising();
    advertising_ = false;
    startAdvertising();
}

void FtmsOutput::transportSetFeature() {
    if (transport_) {
        transport_->feature->setValue(feature_);
        ftms::Features features;
        ftms::decodeFeature(feature_.data(), feature_.size(), features);
        transport_->ranges[0]->setValue(features.targets & ftms::kTargetSpeed
            ? ftms::encodeSupportedSpeedRange() : ftms::Bytes{});
        transport_->ranges[1]->setValue(features.targets & ftms::kTargetInclination
            ? ftms::encodeSupportedInclinationRange() : ftms::Bytes{});
        transport_->ranges[2]->setValue(features.targets & ftms::kTargetResistance
            ? ftms::encodeSupportedResistanceRange() : ftms::Bytes{});
        transport_->ranges[3]->setValue(features.targets & ftms::kTargetPower
            ? ftms::encodeSupportedPowerRange() : ftms::Bytes{});
    }
}

void FtmsOutput::transportApplyProfile() {
    if (!transport_) return;
    // Keep the six data UUIDs stable: rebuilding NimBLE 1.4's service objects
    // would leave its notification callback registry pointing at deleted objects.
    for (auto* characteristic : transport_->machineData) characteristic->setValue(ftms::Bytes{});
    transportSetFeature();
    const auto data = ftms::encodeAdvertisingServiceData(profile_);
    NimBLEDevice::getAdvertising()->setServiceData(NimBLEUUID(ftms::kServiceUuid),
        std::string(data.begin(), data.end()));
    transport_->trainingStatus->setValue(ftms::encodeTrainingStatus(trainingStatus_));
}

std::size_t FtmsOutput::transportMaxPayload() const {
    if (!transport_) return 20;
    std::size_t maximum = 512;
    const auto peers = transport_->server->getPeerDevices();
    if (peers.empty()) return 20;
    for (const auto id : peers) {
        const auto mtu = transport_->server->getPeerMTU(id);
        maximum = std::min(maximum, mtu >= 23 ? static_cast<std::size_t>(mtu - 3) : std::size_t{20});
    }
    return maximum;
}

void FtmsOutput::transportNotifyMachineData() {
    if (!transport_) return;
    auto* characteristic = transport_->machineData[static_cast<std::size_t>(profile_)];
    for (const auto& packet : machineDataPackets_) {
        characteristic->setValue(packet);
        characteristic->notify();
    }
}

void FtmsOutput::transportNotifyTrainingStatus() {
    trainingData_ = ftms::encodeTrainingStatus(trainingStatus_);
    if (!transport_) return;
    transport_->trainingStatus->setValue(trainingData_);
    if (connected_) transport_->trainingStatus->notify();
}

void FtmsOutput::transportIndicateResponse() {
    if (transport_)
        transport_->controlPoint->indicate(response_.data(), response_.size());
}

void FtmsOutput::transportNotifyStatus() {
    if (!transport_) return;
    transport_->machineStatus->setValue(status_);
    transport_->machineStatus->notify();
}

#else

// Host build: no radio. Mirror the stack's observable behaviour for tests.
bool FtmsOutput::transportBegin() { return true; }
bool FtmsOutput::transportStartAdvertising() { return true; }
void FtmsOutput::transportStopAdvertising() {}
void FtmsOutput::transportDisconnectClients() {
    while (connections_.load() > 0) notifyClientDisconnected();
    clearClientAddress();
}
void FtmsOutput::transportApplyName() {}
void FtmsOutput::transportSetFeature() {}
void FtmsOutput::transportApplyProfile() {}
std::size_t FtmsOutput::transportMaxPayload() const { return 20; }
void FtmsOutput::transportNotifyMachineData() {}
void FtmsOutput::transportNotifyTrainingStatus() { trainingData_ = ftms::encodeTrainingStatus(trainingStatus_); }
void FtmsOutput::transportIndicateResponse() {}
void FtmsOutput::transportNotifyStatus() {}

#endif
