#include "ftms_output.h"

#include <chrono>
#include <cmath>
#include <utility>

#ifdef ARDUINO
#include <NimBLEDevice.h>
#include <map>
#endif

using namespace smartfitness;

namespace {
constexpr std::uint32_t kAdvertiseRetryMs = 1000;
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
    void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
        std::lock_guard<std::mutex> lock(subscriptionMutex);
        subscriptions.erase(desc->conn_handle);
    }
    void onWrite(NimBLECharacteristic* characteristic) override {
        const NimBLEAttValue value = characteristic->getValue();
        if (characteristic == cscControl) {
            std::lock_guard<std::mutex> lock(subscriptionMutex);
            if (sensorWrites.size() < kQueueCapacity) {
                PendingWrite write;
                write.length = static_cast<std::uint8_t>(value.length() < write.data.size() ? value.length() : write.data.size());
                write.truncated = value.length() > write.data.size();
                for (std::size_t index = 0; index < write.length; ++index) write.data[index] = value.data()[index];
                write.generation = owner.generation_.load();
                sensorWrites.push_back(write);
            }
            return;
        }
        owner.enqueueControlPoint(value.data(), value.length());
    }
    void onSubscribe(NimBLECharacteristic* characteristic, ble_gap_conn_desc* desc, std::uint16_t value) override {
        std::uint16_t bit = 0;
        for (std::size_t index = 0; index < machineData.size(); ++index)
            if (characteristic == machineData[index]) bit = 1U << index;
        for (std::size_t index = 0; index < sensorData.size(); ++index)
            if (characteristic == sensorData[index]) bit = 1U << (kMachineProfileCount + index);
        if (!bit) return;  // Control/status subscriptions are not data activity.
        std::lock_guard<std::mutex> lock(subscriptionMutex);
        auto& subscribed = subscriptions[desc->conn_handle];
        if (value & 1) subscribed |= bit;
        else subscribed &= ~bit;
    }
    bool subscribed(std::size_t index) const {
        std::lock_guard<std::mutex> lock(subscriptionMutex);
        for (const auto& peer : subscriptions)
            if (peer.second & (1U << index)) return true;
        return false;
    }

    FtmsOutput& owner;
    NimBLEServer* server = nullptr;
    NimBLECharacteristic* feature = nullptr;
    // Machine data characteristics, indexed by MachineProfile.
    std::array<NimBLECharacteristic*, kMachineProfileCount> machineData{};
    NimBLECharacteristic* trainingStatus = nullptr;
    NimBLECharacteristic* controlPoint = nullptr;
    NimBLECharacteristic* machineStatus = nullptr;
    // CPS, HR, CSC, RSC.
    std::array<NimBLECharacteristic*, 4> sensorData{};
    NimBLECharacteristic* cpsFeature = nullptr;
    NimBLECharacteristic* cscFeature = nullptr;
    NimBLECharacteristic* cscControl = nullptr;
    mutable std::mutex subscriptionMutex;
    std::map<std::uint16_t, std::uint16_t> subscriptions;
    std::vector<PendingWrite> sensorWrites;
};
#else
struct FtmsOutput::Transport {};
#endif

FtmsOutput::FtmsOutput() = default;

FtmsOutput::~FtmsOutput() {
#ifdef ARDUINO
    // NimBLE is shared with the input central. Detach our callbacks rather
    // than tearing down its stack (or letting GATT retain a dangling owner).
    if (transport_) {
        transportStopAdvertising();
        transportDisconnectClients();
        transport_->server->setCallbacks(nullptr, false);
        for (auto* characteristic : transport_->machineData) characteristic->setCallbacks(nullptr);
        for (auto* characteristic : transport_->sensorData)
            if (characteristic) characteristic->setCallbacks(nullptr);
        transport_->controlPoint->setCallbacks(nullptr);
        if (transport_->cscControl) transport_->cscControl->setCallbacks(nullptr);
    }
#endif
}

void FtmsOutput::begin() {
    BridgeOutput::begin();
    sensorEncoder_.reset();
    begun_ = true;
    resetSession();
    refreshProfile();
    feature_ = ftms::encodeFeature(trainerState().capabilities, profile_);
    transportSetFeature();
}

void FtmsOutput::loop() {
    handleConnectionChanges();
    drainControlPoint();
    updateTrainingStatus();
    publishBikeData();
    if (enabled()) sensorEncoder_.update(trainerState(), now());
    transportNotifySensors();
}

void FtmsOutput::setEnabled(bool enabled) {
    BridgeOutput::setEnabled(enabled);
    if (!enabled) {
        sensorEncoder_.reset();
        sensorDataSent_ = false;
    }
    if (enabled || !begun_) return;
    transportDisconnectClients();
    stopAdvertising();
    resetSession();
    connected_ = false;
}

void FtmsOutput::publishTrainerState(const TrainerState& state) {
    const bool inputProfileChanged = trainerState().profile != state.profile;
    const bool sourceSessionChanged = inputProfileChanged || trainerState().connected != state.connected;
    const bool lostTreadmill = profile_ == MachineProfile::Treadmill &&
        trainerState().connected &&
        (!state.connected || (trainerState().trainerDetected && !state.trainerDetected));
    BridgeOutput::publishTrainerState(state);
    // Update even without a peripheral client and mark outages immediately.
    // Keep cumulative counts monotonic for an already subscribed central, but
    // never integrate a new source against the previous source's clock/rate.
    if (sourceSessionChanged) sensorEncoder_.invalidate();
    if (enabled()) sensorEncoder_.update(state, now());
    refreshProfile();
    if (lostTreadmill || inputProfileChanged) {
        generation_.fetch_add(1);
        resetSession();
    }
    auto feature = ftms::encodeFeature(state.capabilities, profile_);
    if (feature != feature_) feature_ = std::move(feature);
    transportSetFeature();
}

void FtmsOutput::setProfileOverride(FtmsOutputProfile profile) {
    if (profile == profileOverride_) return;
    generation_.fetch_add(1);
    resetSession();
    profileOverride_ = profile;
    refreshProfile();
    feature_ = ftms::encodeFeature(trainerState().capabilities, profile_);
    transportSetFeature();
}

MachineProfile FtmsOutput::profile() const { return profile_; }

void FtmsOutput::setServices(std::uint8_t rawMask) { configuredServices_ = rawMask; }

std::uint8_t FtmsOutput::services() const {
    const auto selected = topologyFrozen_ ? appliedServices_ : ble::effectiveServices(configuredServices_, profile_);
    return ble::effectiveServices(selected, profile_);
}

bool FtmsOutput::restartRequired() const {
    return topologyFrozen_ && appliedServices_ != ble::effectiveServices(configuredServices_, profile_);
}

bool FtmsOutput::serviceActive(std::uint8_t bit) const {
    if (!enabled() || !connected_ || !(services() & bit) || (bit & (bit - 1))) return false;
#ifdef ARDUINO
    if (!transport_) return false;
    if (bit == ble::kFtms) return transport_->subscribed(static_cast<std::size_t>(profile_));
    for (std::size_t index = 0; index < 4; ++index)
        if (bit == (2U << index)) return transport_->subscribed(kMachineProfileCount + index);
#endif
    return false;  // A host connection alone does not imply a CCCD subscription.
}

void FtmsOutput::refreshProfile() {
    const auto next = effectiveOutputProfile(profileOverride_, trainerState().profile);
    if (next == profile_) return;
    profile_ = next;
    sensorEncoder_.reset();
    sensorDataSent_ = false;
    feature_ = ftms::encodeFeature(trainerState().capabilities, profile_);
    transportSetFeature();
    generation_.fetch_add(1);
    resetSession();
    response_.clear();
    status_.clear();
    for (auto& data : machineData_) data.clear();
    trainingStatus_ = ftms::TrainingStatus::Idle;
    profileDisconnectPending_ = connections_.load() > 0;
    transportDisconnectClients();
    connected_ = false;
    stopAdvertising();
    transportApplyProfile();
    // Initial configuration applies profile and services separately. Only
    // loop() may freeze the first GATT topology, after both have been applied.
    if (topologyFrozen_) startAdvertising();
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
const ftms::Bytes& FtmsOutput::lastIndoorBikeData() const { return lastMachineData(MachineProfile::IndoorBike); }
const ftms::Bytes& FtmsOutput::lastTreadmillData() const { return lastMachineData(MachineProfile::Treadmill); }
const ftms::Bytes& FtmsOutput::lastMachineData(MachineProfile profile) const {
    const auto index = static_cast<std::size_t>(profile);
    return machineData_[index < machineData_.size() ? index : 0];
}
const ftms::Bytes& FtmsOutput::lastControlResponse() const { return response_; }
const ftms::Bytes& FtmsOutput::lastMachineStatus() const { return status_; }
const ftms::Bytes& FtmsOutput::feature() const { return feature_; }
ftms::Bytes FtmsOutput::speedRange() const { return ftms::encodeSupportedSpeedRange(); }
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
        sensorDataSent_ = false;
        trainingStatusSent_ = false;
        clearSessionData();
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
            pending[index].generation == generation_.load() &&
            (profile_ != MachineProfile::Treadmill || connections_.load() > 0))
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
    auto status = ftms::encodeMachineStatus(command);
    if (status.empty()) return;
    status_ = std::move(status);
    transportNotifyStatus();
}

void FtmsOutput::publishBikeData() {
    if (!connected_) return;
    const TrainerState state = trainerState();
    auto payload = ftms::encodeMachineData(profile_, state);
    auto& data = machineData_[static_cast<std::size_t>(profile_) % machineData_.size()];
    const std::uint32_t timestamp = now();
    const std::uint32_t elapsed = timestamp - lastBikeDataMs_;
    if (bikeDataSent_) {
        if (elapsed < kBikeDataMinIntervalMs) return;
        if (payload == data && elapsed < kBikeDataMaxIntervalMs) return;
    }
    data = std::move(payload);
    machineRecords_ = ftms::encodeMachineDataRecords(profile_, state);
    lastBikeDataMs_ = timestamp;
    bikeDataSent_ = true;
    transportNotifyMachineData();
}

// Session data of every non-bike profile restarts with each session.
void FtmsOutput::clearSessionData() {
    for (std::size_t index = 0; index < machineData_.size(); ++index)
        if (index != static_cast<std::size_t>(MachineProfile::IndoorBike)) machineData_[index].clear();
}

void FtmsOutput::updateTrainingStatus() {
    auto status = trainerState().connected ? ftms::TrainingStatus::ManualMode : ftms::TrainingStatus::Idle;
    if (profile_ == MachineProfile::Treadmill) {
        const auto& speed = trainerState().sample(Metric::Speed);
        status = trainerState().connected && speed.valid && std::isfinite(speed.value) && speed.value > 0
            ? ftms::TrainingStatus::ManualMode : ftms::TrainingStatus::Idle;
    }
    if (status == trainingStatus_ &&
        (profile_ != MachineProfile::Treadmill || !connected_ || trainingStatusSent_)) return;
    trainingStatus_ = status;
    transportNotifyTrainingStatus();
    if (profile_ == MachineProfile::Treadmill && connected_) {
        status_ = status == ftms::TrainingStatus::Idle ? ftms::Bytes{0x02, 0x01} : ftms::Bytes{0x04};
        transportNotifyStatus();
        trainingStatusSent_ = true;
    }
}

void FtmsOutput::startAdvertising() {
    if (!begun_ || !enabled() || connected_ || advertising_ || profileDisconnectPending_) return;
    // Auto starts as IndoorBike until the source is identified. Do not freeze
    // away a saved treadmill RSC service before that source can reconnect.
    // Read-only sources count too: trainerDetected is deliberately not used.
    if (!topologyFrozen_ && (configuredServices_ & ble::kRsc) &&
        profileOverride_ == FtmsOutputProfile::Auto && profile_ != MachineProfile::Treadmill &&
        !trainerState().connected) return;
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
    sensorDataSent_ = false;
    trainingStatusSent_ = false;
    clearSessionData();
    std::lock_guard<std::mutex> lock(mutex_);
    queueHead_ = 0;
    queueCount_ = 0;
}

#ifdef ARDUINO

bool FtmsOutput::transportBegin() {
    if (transport_) return true;
    appliedServices_ = ble::effectiveServices(configuredServices_, profile_);
    topologyFrozen_ = true;
    auto transport = std::unique_ptr<Transport>(new Transport(*this));
    // The BLE input (central) may have initialised the shared stack already,
    // in which case init() ignores the name: set it explicitly.
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init(name_);
    NimBLEDevice::setDeviceName(name_);
    transport->server = NimBLEDevice::createServer();
    transport->server->setCallbacks(transport.get(), false);
    // Advertising is restarted from loop() so the enable toggle stays authoritative.
    transport->server->advertiseOnDisconnect(false);

    NimBLEService* service = transport->server->createService(NimBLEUUID(ftms::kServiceUuid));
    transport->feature = service->createCharacteristic(NimBLEUUID(ftms::kFeatureUuid), NIMBLE_PROPERTY::READ);
    for (std::size_t index = 0; index < transport->machineData.size(); ++index) {
        const auto uuid = ftms::machineDataUuid(static_cast<MachineProfile>(index));
        transport->machineData[index] = service->createCharacteristic(NimBLEUUID(uuid), NIMBLE_PROPERTY::NOTIFY);
        transport->machineData[index]->setCallbacks(transport.get());
    }
    transport->trainingStatus = service->createCharacteristic(
        NimBLEUUID(ftms::kTrainingStatusUuid), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* incline = service->createCharacteristic(
        NimBLEUUID(ftms::kSupportedInclinationRangeUuid), NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* speed = service->createCharacteristic(
        NimBLEUUID(ftms::kSupportedSpeedRangeUuid), NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* resistance = service->createCharacteristic(
        NimBLEUUID(ftms::kSupportedResistanceRangeUuid), NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* power = service->createCharacteristic(
        NimBLEUUID(ftms::kSupportedPowerRangeUuid), NIMBLE_PROPERTY::READ);
    transport->controlPoint = service->createCharacteristic(
        NimBLEUUID(ftms::kControlPointUuid), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
    transport->machineStatus = service->createCharacteristic(NimBLEUUID(ftms::kMachineStatusUuid), NIMBLE_PROPERTY::NOTIFY);
    transport->controlPoint->setCallbacks(transport.get());

    transport->feature->setValue(feature_);
    const auto training = ftms::encodeTrainingStatus(trainingStatus_);
    transport->trainingStatus->setValue(training);
    incline->setValue(ftms::encodeSupportedInclinationRange());
    speed->setValue(ftms::encodeSupportedSpeedRange());
    resistance->setValue(ftms::encodeSupportedResistanceRange());
    power->setValue(ftms::encodeSupportedPowerRange());
    service->start();

    auto addSensor = [&](std::size_t index, std::uint16_t serviceUuid, std::uint16_t dataUuid,
                         std::uint16_t featureUuid, const ftms::Bytes& feature) {
        auto* sensor = transport->server->createService(NimBLEUUID(serviceUuid));
        auto* data = sensor->createCharacteristic(NimBLEUUID(dataUuid), NIMBLE_PROPERTY::NOTIFY);
        data->setCallbacks(transport.get());
        transport->sensorData[index] = data;
        if (featureUuid) {
            auto* characteristic = sensor->createCharacteristic(NimBLEUUID(featureUuid), NIMBLE_PROPERTY::READ);
            characteristic->setValue(feature);
            if (index == 0) transport->cpsFeature = characteristic;
            if (index == 2) transport->cscFeature = characteristic;
        }
        if (index == 0 || index == 2) {
            auto* location = sensor->createCharacteristic(
                NimBLEUUID(static_cast<std::uint16_t>(0x2A5D)), NIMBLE_PROPERTY::READ);
            location->setValue(ftms::Bytes{0});  // Other: virtual sensor has no physical location.
        }
        if (index == 2) {
            transport->cscControl = sensor->createCharacteristic(
                NimBLEUUID(static_cast<std::uint16_t>(0x2A55)), NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);
            transport->cscControl->setCallbacks(transport.get());
        }
        sensor->start();
    };
    const auto& capabilities = trainerState().capabilities;
    if (appliedServices_ & ble::kCps)
        addSensor(0, 0x1818, 0x2A63, 0x2A65, sensorEncoder_.cyclingPowerFeature(capabilities, trainerState().profile));
    if (appliedServices_ & ble::kHr) addSensor(1, 0x180D, 0x2A37, 0, {});
    if (appliedServices_ & ble::kCsc)
        addSensor(2, 0x1816, 0x2A5B, 0x2A5C, sensorEncoder_.cyclingSpeedCadenceFeature(capabilities, trainerState().profile));
    if (appliedServices_ & ble::kRsc) addSensor(3, 0x1814, 0x2A53, 0x2A54, ble::SensorEncoder::runningFeature());
    transport_ = std::move(transport);
    transportConfigureAdvertising();
    return true;
}

bool FtmsOutput::transportStartAdvertising() {
    transportConfigureAdvertising();
    return NimBLEDevice::getAdvertising()->start();
}

void FtmsOutput::transportConfigureAdvertising() {
    if (!transport_) return;
    // Explicit legacy packets: flags (3) + five UUIDs (12) + FTMS data (7)
    // fit in 22 bytes. The complete name (at most 22 bytes) is scan-response only.
    NimBLEAdvertisementData primary;
    primary.setFlags(0x06);
    std::vector<NimBLEUUID> uuids{NimBLEUUID(ftms::kServiceUuid)};
    const std::uint16_t sensorUuids[]{0x1818, 0x180D, 0x1816, 0x1814};
    for (std::size_t index = 0; index < 4; ++index)
        if (services() & (2U << index)) uuids.emplace_back(sensorUuids[index]);
    primary.setCompleteServices16(uuids);
    const auto serviceData = ftms::encodeAdvertisingServiceData(profile_);
    primary.setServiceData(NimBLEUUID(ftms::kServiceUuid), std::string(serviceData.begin(), serviceData.end()));
    NimBLEAdvertisementData scan;
    scan.setName(name_);
    auto* advertising = NimBLEDevice::getAdvertising();
    advertising->setScanResponse(true);
    advertising->setAdvertisementData(primary);
    advertising->setScanResponseData(scan);
}

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
    const bool wasAdvertising = advertising_;
    if (wasAdvertising) transportStopAdvertising();
    transportConfigureAdvertising();
    if (!wasAdvertising) return;
    advertising_ = false;
    startAdvertising();
}

void FtmsOutput::transportSetFeature() {
    if (!transport_) return;
    transport_->feature->setValue(feature_);
    if (transport_->cpsFeature)
        transport_->cpsFeature->setValue(sensorEncoder_.cyclingPowerFeature(trainerState().capabilities, trainerState().profile));
    if (transport_->cscFeature)
        transport_->cscFeature->setValue(sensorEncoder_.cyclingSpeedCadenceFeature(trainerState().capabilities, trainerState().profile));
}

void FtmsOutput::transportApplyProfile() {
    if (!transport_) return;
    transportConfigureAdvertising();
    transport_->trainingStatus->setValue(ftms::encodeTrainingStatus(trainingStatus_));
}

void FtmsOutput::transportNotifyMachineData() {
    if (!transport_) return;
    auto* characteristic = transport_->machineData[static_cast<std::size_t>(profile_) % machineData_.size()];
    for (const auto& record : machineRecords_) {
        characteristic->setValue(record);
        characteristic->notify();
    }
}

void FtmsOutput::transportNotifyTrainingStatus() {
    if (!transport_) return;
    transport_->trainingStatus->setValue(ftms::encodeTrainingStatus(trainingStatus_));
    transport_->trainingStatus->notify();
}

void FtmsOutput::transportIndicateResponse() {
    if (transport_) transport_->controlPoint->indicate(response_.data(), response_.size());
}

void FtmsOutput::transportNotifyStatus() {
    if (!transport_) return;
    transport_->machineStatus->setValue(status_);
    transport_->machineStatus->notify();
}

void FtmsOutput::transportNotifySensors() {
    if (!transport_) return;
    std::vector<PendingWrite> writes;
    {
        std::lock_guard<std::mutex> lock(transport_->subscriptionMutex);
        writes.swap(transport_->sensorWrites);
    }
    for (const auto& pending : writes) {
        if (pending.length == 0 || !connected_ || pending.generation != generation_.load()) continue;
        const auto& write = pending.data;
        std::uint8_t result = 2;  // Opcode not supported.
        if (write[0] == 1 && trainerState().profile == MachineProfile::IndoorBike &&
            trainerState().capabilities.supports(Metric::Speed)) {
            result = pending.length == 5 && !pending.truncated ? 1 : 3;
            if (result == 1) {
                std::uint32_t value = 0;
                for (std::size_t index = 0; index < 4; ++index)
                    value |= static_cast<std::uint32_t>(write[index + 1]) << (index * 8);
                sensorEncoder_.setWheelRevolutions(value);
            }
        }
        transport_->cscControl->indicate(ftms::Bytes{0x10, write[0], result});
    }
    if (!enabled() || !connected_) return;
    const auto timestamp = now();
    if (sensorDataSent_ && timestamp - lastSensorDataMs_ < kBikeDataMinIntervalMs) return;
    lastSensorDataMs_ = timestamp;
    sensorDataSent_ = true;
    const auto& state = trainerState();
    const std::array<ftms::Bytes, 4> records{
        sensorEncoder_.cyclingPower(state), sensorEncoder_.heartRate(state),
        sensorEncoder_.cyclingSpeedCadence(state), sensorEncoder_.runningSpeedCadence(state)};
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto* characteristic = transport_->sensorData[index];
        if (!characteristic || !(services() & (2U << index)) || records[index].empty()) continue;
        characteristic->setValue(records[index]);
        characteristic->notify();
    }
}

#else

// Host build: no radio. Mirror the stack's observable behaviour for tests.
bool FtmsOutput::transportBegin() {
    if (!topologyFrozen_) {
        appliedServices_ = ble::effectiveServices(configuredServices_, profile_);
        topologyFrozen_ = true;
    }
    return true;
}
bool FtmsOutput::transportStartAdvertising() { return true; }
void FtmsOutput::transportStopAdvertising() {}
void FtmsOutput::transportDisconnectClients() {
    while (connections_.load() > 0) notifyClientDisconnected();
    clearClientAddress();
}
void FtmsOutput::transportApplyName() {}
void FtmsOutput::transportSetFeature() {}
void FtmsOutput::transportApplyProfile() {}
void FtmsOutput::transportNotifyMachineData() {}
void FtmsOutput::transportNotifyTrainingStatus() {}
void FtmsOutput::transportIndicateResponse() {}
void FtmsOutput::transportNotifyStatus() {}
void FtmsOutput::transportNotifySensors() {}
void FtmsOutput::transportConfigureAdvertising() {}

#endif
