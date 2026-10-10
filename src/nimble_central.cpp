#ifdef ARDUINO

#include "nimble_central.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <type_traits>

using namespace smartfitness;

namespace {
NimbleCentral* gCentral = nullptr;

NimBLEUUID toNimble(const ble::Uuid& uuid) {
    if (uuid.isShort()) return NimBLEUUID(uuid.shortValue());
    return NimBLEUUID(uuid.toCanonicalString());
}

NimBLERemoteCharacteristic* findCharacteristic(NimBLEClient* client, const ble::Uuid& service,
                                               const ble::Uuid& characteristic) {
    NimBLERemoteService* remote = client->getService(toNimble(service));
    return remote ? remote->getCharacteristic(toNimble(characteristic)) : nullptr;
}
}  // namespace

struct NimbleCentral::Impl : public NimBLEAdvertisedDeviceCallbacks, public NimBLEClientCallbacks {
    explicit Impl(NimbleCentral& central) : owner(central) {}

    // Host task: convert and queue, nothing else.
    void onResult(NimBLEAdvertisedDevice* device) override {
        ble::ScanReport report;
        if (!ble::BleAddress::parse(device->getAddress().toString(), report.address, device->getAddressType())) return;
        report.rssi = static_cast<std::int8_t>(device->getRSSI());
        report.connectable = device->isConnectable();
        if (device->haveName()) report.setName(device->getName());
        for (std::uint8_t index = 0; index < device->getServiceUUIDCount(); ++index) {
            ble::Uuid uuid;
            if (ble::Uuid::parse(device->getServiceUUID(index).toString(), uuid)) report.addService(uuid);
        }
        if (device->getManufacturerDataCount() > 0) {
            const std::string data = device->getManufacturerData();
            report.setManufacturerData(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
        }
        owner.scans_.push(report);
    }

    // Called by NimBLE when a timed scan ends or stop() is called.
    static void scanComplete(NimBLEScanResults) {
        if (gCentral && gCentral->scanning_.exchange(false))
            gCentral->pushEvent(ble::CentralEventType::ScanComplete, 0);
    }

    void onDisconnect(NimBLEClient*) override {
        owner.pushEvent(ble::CentralEventType::Disconnected, owner.session_.load());
    }

    NimbleCentral& owner;
    NimBLEClient* client = nullptr;
};

bool NimbleCentral::begin() {
    if (impl_) return true;
    static_assert(std::is_trivially_copyable<Cmd>::value, "commands are copied through a FreeRTOS queue");
    // The FTMS output (peripheral) may initialise the stack first; both roles share it.
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init("");
    impl_ = new Impl(*this);
    gCentral = this;
    ble::BleAddress::parse(NimBLEDevice::getAddress().toString(), own_);

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(97);
    scan->setWindow(37);  // leave radio time for the peripheral role and Wi-Fi coexistence
    scan->setMaxResults(0);  // results are only delivered through the callback

    impl_->client = NimBLEDevice::createClient();
    impl_->client->setClientCallbacks(impl_, false);
    impl_->client->setConnectTimeout(kConnectTimeoutS);

    queue_ = xQueueCreate(kCommandQueue, sizeof(Cmd));
    if (!queue_) return false;
    return xTaskCreatePinnedToCore(workerEntry, "ble-central", 6144, this, 1, nullptr, 1) == pdPASS;
}

bool NimbleCentral::send(const Cmd& cmd) {
    return queue_ && xQueueSend(static_cast<QueueHandle_t>(queue_), &cmd, 0) == pdTRUE;
}

bool NimbleCentral::startScan(std::uint32_t durationMs, bool duplicates) {
    Cmd cmd;
    cmd.op = Op::StartScan;
    cmd.durationMs = durationMs;
    cmd.duplicates = duplicates;
    if (!send(cmd)) return false;
    scanning_ = true;
    return true;
}

void NimbleCentral::stopScan() {
    Cmd cmd;
    cmd.op = Op::StopScan;
    send(cmd);
}

bool NimbleCentral::connect(std::uint32_t session, const ble::BleAddress& address, const ble::ConnectPlan& plan) {
    Cmd cmd;
    cmd.op = Op::Connect;
    cmd.session = session;
    cmd.address = address;
    cmd.plan = plan;
    return send(cmd);
}

void NimbleCentral::disconnect(std::uint32_t session) {
    Cmd cmd;
    cmd.op = Op::Disconnect;
    cmd.session = session;
    send(cmd);
}

bool NimbleCentral::write(std::uint32_t session, const ble::GattWrite& write) {
    Cmd cmd;
    cmd.op = Op::Write;
    cmd.session = session;
    cmd.write = write;
    return send(cmd);
}

bool NimbleCentral::pollScan(ble::ScanReport& report) { return scans_.pop(report); }
bool NimbleCentral::pollEvent(ble::CentralEvent& event) { return events_.pop(event); }

void NimbleCentral::pushEvent(ble::CentralEventType type, std::uint32_t session, std::uint8_t index,
                              std::uint32_t mask, const std::uint8_t* data, std::size_t length) {
    ble::CentralEvent event;
    event.type = type;
    event.session = session;
    event.index = index;
    event.mask = mask;
    if (data) event.setData(data, length);
    events_.push(event);
}

void NimbleCentral::workerEntry(void* self) { static_cast<NimbleCentral*>(self)->worker(); }

void NimbleCentral::worker() {
    Cmd cmd;
    for (;;) {
        if (xQueueReceive(static_cast<QueueHandle_t>(queue_), &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.op) {
            case Op::StartScan: doStartScan(cmd); break;
            case Op::StopScan:
                if (NimBLEDevice::getScan()->isScanning()) NimBLEDevice::getScan()->stop();
                if (scanning_.exchange(false)) pushEvent(ble::CentralEventType::ScanComplete, 0);
                break;
            case Op::Connect: doConnect(cmd); break;
            case Op::Disconnect:
                if (cmd.session == session_.load()) doDisconnect();
                break;
            case Op::Write: doWrite(cmd); break;
        }
    }
}

void NimbleCentral::doStartScan(const Cmd& cmd) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan->isScanning()) {
        scanning_ = false;  // a restart is not a completed scan: suppress the event
        scan->stop();
    }
    scan->setAdvertisedDeviceCallbacks(impl_, cmd.duplicates);
    scan->setDuplicateFilter(!cmd.duplicates);
    // NimBLE-Arduino 1.4.x takes seconds; 0 scans until stopped.
    const std::uint32_t seconds = cmd.durationMs == 0 ? 0 : (cmd.durationMs + 999) / 1000;
    if (!scan->start(seconds, Impl::scanComplete, false)) {
        if (scanning_.exchange(false)) pushEvent(ble::CentralEventType::ScanComplete, 0);
        return;
    }
    scanning_ = true;
}

void NimbleCentral::doConnect(const Cmd& cmd) {
    session_ = cmd.session;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan->isScanning()) {
        scan->stop();
        if (scanning_.exchange(false)) pushEvent(ble::CentralEventType::ScanComplete, 0);
    }
    NimBLEClient* client = impl_->client;
    if (client->isConnected()) client->disconnect();
    const NimBLEAddress address(cmd.address.toString(), cmd.address.type);
    if (!client->connect(address, true)) {
        pushEvent(ble::CentralEventType::ConnectFailed, cmd.session);
        return;
    }
    // Reads are reported before Connected so protocols can derive capabilities.
    for (std::uint8_t index = 0; index < cmd.plan.readCount; ++index) {
        const auto& read = cmd.plan.reads[index];
        NimBLERemoteCharacteristic* characteristic = findCharacteristic(client, read.service, read.characteristic);
        if (!characteristic || !characteristic->canRead()) continue;
        const NimBLEAttValue value = characteristic->readValue();
        pushEvent(ble::CentralEventType::ReadResult, cmd.session, index, 0, value.data(), value.length());
    }
    std::uint32_t mask = 0;
    const std::uint32_t session = cmd.session;
    for (std::uint8_t index = 0; index < cmd.plan.subscriptionCount; ++index) {
        const auto& subscription = cmd.plan.subscriptions[index];
        NimBLERemoteCharacteristic* characteristic =
            findCharacteristic(client, subscription.service, subscription.characteristic);
        if (!characteristic) continue;
        const bool supported = subscription.indicate ? characteristic->canIndicate() : characteristic->canNotify();
        if (!supported) continue;
        // Runs on the NimBLE host task: queue only.
        auto callback = [this, session, index](NimBLERemoteCharacteristic*, std::uint8_t* data, std::size_t length,
                                               bool) {
            pushEvent(ble::CentralEventType::Notification, session, index, 0, data, length);
        };
        if (characteristic->subscribe(!subscription.indicate, callback, true)) mask |= 1UL << index;
    }
    if (!client->isConnected()) {
        pushEvent(ble::CentralEventType::ConnectFailed, cmd.session);
        return;
    }
    pushEvent(ble::CentralEventType::Connected, cmd.session, 0, mask);
}

void NimbleCentral::doDisconnect() {
    if (impl_->client->isConnected()) impl_->client->disconnect();
}

void NimbleCentral::doWrite(const Cmd& cmd) {
    NimBLEClient* client = impl_->client;
    if (cmd.session != session_.load() || !client->isConnected()) {
        pushEvent(ble::CentralEventType::WriteFailed, cmd.session);
        return;
    }
    NimBLERemoteCharacteristic* characteristic =
        findCharacteristic(client, cmd.write.service, cmd.write.characteristic);
    if (!characteristic) {
        pushEvent(ble::CentralEventType::WriteFailed, cmd.session);
        return;
    }
    bool response = cmd.write.withResponse;
    // Vendor characteristics often only allow one of the two write types.
    if (response && !characteristic->canWrite() && characteristic->canWriteNoResponse()) response = false;
    if (!response && !characteristic->canWriteNoResponse() && characteristic->canWrite()) response = true;
    if (!characteristic->writeValue(cmd.write.data.data(), cmd.write.length, response))
        pushEvent(ble::CentralEventType::WriteFailed, cmd.session);
}

#endif  // ARDUINO
