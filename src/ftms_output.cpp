#include "ftms_output.h"

#include <chrono>
#include <utility>

#ifdef ARDUINO
#include <NimBLEDevice.h>
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
    void onWrite(NimBLECharacteristic* characteristic) override {
        const NimBLEAttValue value = characteristic->getValue();
        owner.enqueueControlPoint(value.data(), value.length());
    }

    FtmsOutput& owner;
    NimBLEServer* server = nullptr;
    NimBLECharacteristic* feature = nullptr;
    NimBLECharacteristic* bikeData = nullptr;
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
    feature_ = ftms::encodeFeature(trainerState().capabilities);
    transportSetFeature();
}

void FtmsOutput::loop() {
    handleConnectionChanges();
    drainControlPoint();
    updateTrainingStatus();
    publishBikeData();
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
    BridgeOutput::publishTrainerState(state);
    auto feature = ftms::encodeFeature(state.capabilities);
    if (feature == feature_) return;
    feature_ = std::move(feature);
    transportSetFeature();
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
const ftms::Bytes& FtmsOutput::lastControlResponse() const { return response_; }
const ftms::Bytes& FtmsOutput::lastMachineStatus() const { return status_; }
const ftms::Bytes& FtmsOutput::feature() const { return feature_; }
void FtmsOutput::setClock(Clock clock) { clock_ = std::move(clock); }

std::uint32_t FtmsOutput::now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return static_cast<std::uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void FtmsOutput::handleConnectionChanges() {
    connected_ = enabled() && connections_.load() > 0;
    const std::uint32_t generation = generation_.load();
    if (generation != seenGeneration_) {
        // Every connect or disconnect starts a new session: control must be requested again.
        seenGeneration_ = generation;
        granted_ = false;
        bikeDataSent_ = false;
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
        if (connected_ && pending[index].generation == seenGeneration_) handleControlPoint(pending[index]);
    }
}

void FtmsOutput::handleControlPoint(const PendingWrite& write) {
    ftms::ControlPointCommand command;
    if (!ftms::decodeControlPoint(write.data.data(), write.length, command)) return;
    ftms::ResultCode result = write.truncated ? ftms::ResultCode::InvalidParameter : command.status;
    if (result == ftms::ResultCode::Success) {
        if (command.opcode == static_cast<std::uint8_t>(ftms::OpCode::RequestControl)) {
            granted_ = true;
        } else if (!granted_) {
            result = ftms::ResultCode::ControlNotPermitted;
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
    auto payload = ftms::encodeIndoorBikeData(ftms::indoorBikeDataFrom(trainerState()));
    const std::uint32_t timestamp = now();
    const std::uint32_t elapsed = timestamp - lastBikeDataMs_;
    if (bikeDataSent_) {
        if (elapsed < kBikeDataMinIntervalMs) return;
        if (payload == bikeData_ && elapsed < kBikeDataMaxIntervalMs) return;
    }
    bikeData_ = std::move(payload);
    lastBikeDataMs_ = timestamp;
    bikeDataSent_ = true;
    transportNotifyBikeData();
}

void FtmsOutput::updateTrainingStatus() {
    const auto status = trainerState().connected ? ftms::TrainingStatus::ManualMode : ftms::TrainingStatus::Idle;
    if (status == trainingStatus_) return;
    trainingStatus_ = status;
    transportNotifyTrainingStatus();
}

void FtmsOutput::startAdvertising() {
    if (!begun_ || !enabled() || connected_ || advertising_) return;
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
    std::lock_guard<std::mutex> lock(mutex_);
    queueHead_ = 0;
    queueCount_ = 0;
}

#ifdef ARDUINO

bool FtmsOutput::transportBegin() {
    if (transport_) return true;
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
    transport->bikeData = service->createCharacteristic(NimBLEUUID(ftms::kIndoorBikeDataUuid), NIMBLE_PROPERTY::NOTIFY);
    transport->trainingStatus = service->createCharacteristic(
        NimBLEUUID(ftms::kTrainingStatusUuid), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* incline = service->createCharacteristic(
        NimBLEUUID(ftms::kSupportedInclinationRangeUuid), NIMBLE_PROPERTY::READ);
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
    resistance->setValue(ftms::encodeSupportedResistanceRange());
    power->setValue(ftms::encodeSupportedPowerRange());
    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(NimBLEUUID(ftms::kServiceUuid));
    const auto serviceData = ftms::encodeAdvertisingServiceData();
    advertising->setServiceData(NimBLEUUID(ftms::kServiceUuid), std::string(serviceData.begin(), serviceData.end()));
    advertising->setScanResponse(true);
    advertising->setName(name_);
    transport_ = std::move(transport);
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
    if (transport_) transport_->feature->setValue(feature_);
}

void FtmsOutput::transportNotifyBikeData() {
    if (!transport_) return;
    transport_->bikeData->setValue(bikeData_);
    transport_->bikeData->notify();
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

#else

// Host build: no radio. Mirror the stack's observable behaviour for tests.
bool FtmsOutput::transportBegin() { return true; }
bool FtmsOutput::transportStartAdvertising() { return true; }
void FtmsOutput::transportStopAdvertising() {}
void FtmsOutput::transportDisconnectClients() {
    if (connections_.load() > 0) notifyClientDisconnected();
}
void FtmsOutput::transportApplyName() {}
void FtmsOutput::transportSetFeature() {}
void FtmsOutput::transportNotifyBikeData() {}
void FtmsOutput::transportNotifyTrainingStatus() {}
void FtmsOutput::transportIndicateResponse() {}
void FtmsOutput::transportNotifyStatus() {}

#endif
