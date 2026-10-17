#ifdef ARDUINO

#include "nimble_central.h"
#include "ble/connection_session.hpp"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <type_traits>

using namespace smartfitness;

namespace {
NimbleCentral* gCentral = nullptr;

NimBLEUUID toNimble(const ble::Uuid& uuid) {
    if (uuid.isShort()) return NimBLEUUID(uuid.shortValue());
    return NimBLEUUID(uuid.toCanonicalString());
}

NimBLERemoteCharacteristic* findCached(NimBLEClient* client, const ble::Uuid& service,
                                      const ble::Uuid& characteristic) {
    const NimBLEUUID serviceUuid = toNimble(service);
    const NimBLEUUID characteristicUuid = toNimble(characteristic);
    for (auto* remote : *client) {
        if (remote->getUUID() != serviceUuid) continue;
        for (auto* cached : *remote)
            if (cached->getUUID() == characteristicUuid) return cached;
    }
    return nullptr;
}

// Generic Access / Generic Attribute say nothing about the device.
bool genericService(const ble::Uuid& uuid) {
    return uuid == ble::Uuid::from16(0x1800) || uuid == ble::Uuid::from16(0x1801);
}
}  // namespace

struct NimbleCentral::Impl : public NimBLEAdvertisedDeviceCallbacks, public NimBLEClientCallbacks {
    explicit Impl(NimbleCentral& central) : owner(central) {}

    // Host task: convert and queue, nothing else.
    void onResult(NimBLEAdvertisedDevice* device) override {
        ble::ScanReport report;
        if (!ble::BleAddress::parse(device->getAddress().toString(), report.address, device->getAddressType())) return;
        {
            std::lock_guard<std::mutex> lock(scanMutex);
            if (probing && report.address != probeAddress) return;
        }
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
        for (std::uint8_t index = 0; index < device->getServiceDataCount(); ++index) {
            if (device->getServiceDataUUID(index) != NimBLEUUID(static_cast<std::uint16_t>(0x1826))) continue;
            const std::string data = device->getServiceData(index);
            report.setFtmsServiceData(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
            report.addService(ble::Uuid::from16(0x1826));
        }
        owner.scans_.push(report);
    }

    // Called by NimBLE when a timed scan ends or stop() is called.
    static void scanComplete(NimBLEScanResults) {
        NimBLEDevice::getScan()->setWindow(kScanWindowNormal);
        if (gCentral && gCentral->scanning_.exchange(false))
            gCentral->pushEvent(ble::CentralEventType::ScanComplete, 0);
    }

    void onConnect(NimBLEClient* client) override {
        std::lock_guard<std::mutex> lock(callbackMutex);
        if (client->isConnected()) connectedSession.connected(owner.session_.load());
    }

    void onDisconnect(NimBLEClient* client) override {
        std::lock_guard<std::mutex> lock(callbackMutex);
        // Capture the link identity before releasing it for reuse. A queued
        // disconnect must never acquire the next connection's session.
        const std::uint32_t session = connectedSession.disconnected();
        if (session != 0)
            owner.pushEvent(ble::CentralEventType::Disconnected, session, 0, 0, nullptr, 0,
                            static_cast<std::uint16_t>(client->getLastError()));
    }

    NimbleCentral& owner;
    NimBLEClient* client = nullptr;
    ble::ConnectionSession connectedSession;
    std::mutex callbackMutex;
    std::mutex scanMutex;
    bool probing = false;
    ble::BleAddress probeAddress;
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
    scan->setInterval(kScanInterval);
    scan->setWindow(kScanWindowNormal);  // leave radio time for the peripheral role and Wi-Fi coexistence
    scan->setMaxResults(0);  // results are only delivered through the callback

    impl_->client = NimBLEDevice::createClient();
    if (!impl_->client) {
        delete impl_;
        impl_ = nullptr;
        gCentral = nullptr;
        return false;
    }
    impl_->client->setClientCallbacks(impl_, false);
    impl_->client->setConnectTimeout(kConnectTimeoutS);

    queue_ = xQueueCreate(kCommandQueue, sizeof(Cmd));
    if (queue_ && xTaskCreatePinnedToCore(workerEntry, "ble-central", 6144, this, 1, nullptr, 1) == pdPASS)
        return true;
    if (queue_) vQueueDelete(static_cast<QueueHandle_t>(queue_));
    queue_ = nullptr;
    NimBLEDevice::deleteClient(impl_->client);
    delete impl_;
    impl_ = nullptr;
    gCentral = nullptr;
    return false;
}

bool NimbleCentral::send(const Cmd& cmd) {
    if (queue_ && xQueueSend(static_cast<QueueHandle_t>(queue_), &cmd, 0) == pdTRUE) return true;
    ++droppedCommands_;
    return false;
}

bool NimbleCentral::startScan(std::uint32_t durationMs, bool duplicates, bool highDuty) {
    return queueScan(durationMs, duplicates, highDuty, nullptr);
}

bool NimbleCentral::startProbe(const ble::BleAddress& address, std::uint32_t durationMs) {
    return queueScan(durationMs, true, true, &address);
}

bool NimbleCentral::queueScan(std::uint32_t durationMs, bool duplicates, bool highDuty,
                              const ble::BleAddress* target) {
    Cmd cmd;
    cmd.op = Op::StartScan;
    cmd.durationMs = durationMs;
    cmd.duplicates = duplicates;
    cmd.highDuty = highDuty;
    cmd.probe = target != nullptr;
    if (target) cmd.address = *target;
    std::lock_guard<std::mutex> lock(commandMutex_);
    cmd.session = scanGeneration_ + 1;
    const bool wasScanning = scanning_.exchange(true);
    if (!send(cmd)) {
        scanning_ = wasScanning;
        return false;
    }
    scanGeneration_ = cmd.session;
    return true;
}

bool NimbleCentral::stopScan() {
    Cmd cmd;
    cmd.op = Op::StopScan;
    std::lock_guard<std::mutex> lock(commandMutex_);
    const std::uint32_t pending = pendingConnection_.cancel();
    if (pending != 0) {
        std::uint32_t expected = pending;
        requestedSession_.compare_exchange_strong(expected, 0);
        if (session_.load() == pending && impl_ && impl_->client->isConnected())
            impl_->client->disconnect();
        pushEvent(ble::CentralEventType::ConnectFailed, pending);
    }
    cmd.session = ++scanGeneration_;  // invalidate scan starts waiting behind a connect
    const bool wasScanning = scanning_.exchange(false);
    if (impl_ && NimBLEDevice::getScan()->isScanning()) {
        NimBLEDevice::getScan()->stop();
        NimBLEDevice::getScan()->setWindow(kScanWindowNormal);
    }
    if (wasScanning) pushEvent(ble::CentralEventType::ScanComplete, 0);
    return send(cmd);
}

bool NimbleCentral::connect(std::uint32_t session, const ble::BleAddress& address, const ble::ConnectPlan& plan) {
    Cmd cmd;
    cmd.op = Op::Connect;
    cmd.session = session;
    cmd.address = address;
    cmd.plan = plan;
    std::lock_guard<std::mutex> lock(commandMutex_);
    if (!send(cmd)) return false;
    requestedSession_ = session;
    pendingConnection_.start(session);
    return true;
}

bool NimbleCentral::disconnect(std::uint32_t session) {
    Cmd cmd;
    cmd.op = Op::Disconnect;
    cmd.session = session;
    // Cancellation is out-of-band: it still works with a full command queue
    // and is observed after NimBLE 1.4.3's bounded connect wait (timeout + 1s).
    std::lock_guard<std::mutex> lock(commandMutex_);
    std::uint32_t expected = session;
    requestedSession_.compare_exchange_strong(expected, 0);
    pendingConnection_.complete(session);
    if (session == session_.load() && impl_ && impl_->client->isConnected())
        impl_->client->disconnect();  // also releases pending GATT operations
    return send(cmd);
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
                              std::uint32_t mask, const std::uint8_t* data, std::size_t length,
                              std::uint16_t reasonCode) {
    ble::CentralEvent event;
    event.type = type;
    event.session = session;
    event.index = index;
    event.mask = mask;
    event.reasonCode = reasonCode;
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
            case Op::StopScan: {
                std::lock_guard<std::mutex> lock(commandMutex_);
                if (cmd.session != scanGeneration_) break;
                if (NimBLEDevice::getScan()->isScanning()) NimBLEDevice::getScan()->stop();
                if (scanning_.exchange(false)) pushEvent(ble::CentralEventType::ScanComplete, 0);
                break;
            }
            case Op::Connect: doConnect(cmd); break;
            case Op::Disconnect:
                if (cmd.session == session_.load()) doDisconnect();
                break;
            case Op::Write: doWrite(cmd); break;
        }
    }
}

void NimbleCentral::doStartScan(const Cmd& cmd) {
    std::lock_guard<std::mutex> lock(commandMutex_);
    if (cmd.session != scanGeneration_) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan->isScanning()) {
        scanning_ = false;  // a restart is not a completed scan: suppress the event
        scan->stop();
    }
    scan->setInterval(kScanInterval);
    scan->setWindow(cmd.highDuty ? kScanWindowHighDuty : kScanWindowNormal);
    {
        std::lock_guard<std::mutex> scanLock(impl_->scanMutex);
        impl_->probing = cmd.probe;
        impl_->probeAddress = cmd.address;
    }
    scan->setAdvertisedDeviceCallbacks(impl_, cmd.duplicates);
    scan->setDuplicateFilter(!cmd.duplicates);
    // NimBLE-Arduino 1.4.x takes seconds; 0 scans until stopped.
    const std::uint32_t seconds = cmd.durationMs / 1000 + (cmd.durationMs % 1000 != 0);
    scanning_ = true;
    if (!scan->start(seconds, Impl::scanComplete, false)) {
        scan->setWindow(kScanWindowNormal);
        if (scanning_.exchange(false)) pushEvent(ble::CentralEventType::ScanComplete, 0);
        return;
    }
}

void NimbleCentral::doConnect(const Cmd& cmd) {
    struct PendingGuard {
        ble::PendingConnection& pending;
        std::uint32_t session;
        ~PendingGuard() { pending.complete(session); }
    } pendingGuard{pendingConnection_, cmd.session};
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (cancelled(cmd.session)) return;
    }
    NimBLEClient* client = impl_->client;
    doDisconnect();
    const std::uint32_t waitStarted = millis();
    while (client->isConnected() || impl_->connectedSession.active() != 0) {
        if (cancelled(cmd.session)) return;
        if (millis() - waitStarted >= kConnectTimeoutS * 1000UL) {
            pushEvent(ble::CentralEventType::ConnectFailed, cmd.session);
            return;  // never reuse a client whose old disconnect is outstanding
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (cancelled(cmd.session)) return;
        session_ = cmd.session;
    }
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (cancelled(cmd.session)) return;
        NimBLEScan* scan = NimBLEDevice::getScan();
        if (scan->isScanning()) {
            scan->stop();
            scan->setWindow(kScanWindowNormal);
            if (scanning_.exchange(false)) pushEvent(ble::CentralEventType::ScanComplete, 0);
        }
    }
    if (cancelled(cmd.session)) return;
    const NimBLEAddress address(cmd.address.toString(), cmd.address.type);
    pushEvent(ble::CentralEventType::ConnectProgress, cmd.session,
              static_cast<std::uint8_t>(ble::ConnectPhase::Connect));
    if (!client->connect(address, true)) {
        pushEvent(ble::CentralEventType::ConnectFailed, cmd.session, ble::kConnectFailure, 0, nullptr, 0,
                  static_cast<std::uint16_t>(client->getLastError()));
        return;
    }
    if (cancelled(cmd.session)) {
        doDisconnect();
        return;
    }
    auto progress = [this, &cmd](ble::ConnectPhase phase) {
        pushEvent(ble::CentralEventType::ConnectProgress, cmd.session, static_cast<std::uint8_t>(phase));
    };
    auto discoveryFailed = [this, client, &cmd]() {
        if (!cancelled(cmd.session))
            pushEvent(ble::CentralEventType::ConnectFailed, cmd.session, ble::kServiceDiscoveryFailure,
                      0, nullptr, 0, static_cast<std::uint16_t>(client->getLastError()));
        doDisconnect();
    };
    progress(ble::ConnectPhase::Discovery);
    // In NimBLE 1.4.3 the vector refresh APIs discard characteristic discovery
    // errors. This public operation returns failure and discovers each service,
    // characteristic and descriptor once; all later access is cache-only.
    const bool discovered = client->discoverAttributes();
    if (cancelled(cmd.session)) { doDisconnect(); return; }
    progress(ble::ConnectPhase::Discovery);
    if (!discovered || !client->isConnected()) {
        discoveryFailed();
        return;
    }
    const auto* services = client->getServices(false);
    // Reads are reported before Connected so protocols can derive capabilities.
    for (std::uint8_t index = 0; index < cmd.plan.readCount; ++index) {
        if (cancelled(cmd.session)) { doDisconnect(); return; }
        const auto& read = cmd.plan.reads[index];
        NimBLERemoteCharacteristic* characteristic = findCached(client, read.service, read.characteristic);
        if (!characteristic || !characteristic->canRead()) continue;
        progress(ble::ConnectPhase::Read);
        const NimBLEAttValue value = characteristic->readValue();
        if (cancelled(cmd.session)) { doDisconnect(); return; }
        pushEvent(ble::CentralEventType::ReadResult, cmd.session, index, 0, value.data(), value.length());
    }
    std::uint32_t mask = 0;
    const std::uint32_t session = cmd.session;
    ble::ConnectPlan plan = cmd.plan;
    // Results are reported to the adapter (main loop) before Connected; the
    // host task never logs.
    auto reportSubscription = [this, session](std::uint8_t index, const ble::Subscription& subscription,
                                              ble::SubscriptionOutcome outcome) {
        std::uint8_t data[32];
        for (std::size_t i = 0; i < 16; ++i) {
            data[i] = subscription.service.bytes[i];
            data[i + 16] = subscription.characteristic.bytes[i];
        }
        pushEvent(ble::CentralEventType::SubscriptionResult, session, index,
                  subscription.indicate ? ble::kSubscriptionIndicate : 0U, data, sizeof(data),
                  static_cast<std::uint16_t>(outcome));
    };
    std::uint8_t overflowCount = 0;
    ble::Subscription firstOverflow;
    if (plan.discoverNotifications) {
        plan.subscriptionCount = 0;
        bool writeFound = false;
        for (auto* service : *services) {
            if (cancelled(session)) { doDisconnect(); return; }
            ble::Uuid serviceUuid;
            if (!ble::Uuid::parse(service->getUUID().toString(), serviceUuid)) continue;
            for (auto* characteristic : *service) {
                if (cancelled(session)) { doDisconnect(); return; }
                ble::Uuid characteristicUuid;
                if (!ble::Uuid::parse(characteristic->getUUID().toString(), characteristicUuid)) continue;
                if (characteristic->canNotify() || characteristic->canIndicate()) {
                    const bool indicate = !characteristic->canNotify();
                    if (!plan.subscribe(serviceUuid, characteristicUuid, indicate)) {
                        if (overflowCount == 0) firstOverflow = {serviceUuid, characteristicUuid, indicate};
                        if (overflowCount < 0xff) ++overflowCount;
                    }
                }
                bool allowed = false;
                for (const auto& uuid : plan.discoveryWriteCharacteristics)
                    allowed = allowed || (!uuid.isZero() && uuid == characteristicUuid);
                if (!writeFound && allowed && (characteristic->canWrite() || characteristic->canWriteNoResponse())) {
                    ble::Bytes endpoint(serviceUuid.bytes.begin(), serviceUuid.bytes.end());
                    endpoint.insert(endpoint.end(), characteristicUuid.bytes.begin(), characteristicUuid.bytes.end());
                    pushEvent(ble::CentralEventType::WriteEndpoint, session, 0, 0, endpoint.data(), endpoint.size());
                    writeFound = true;
                }
            }
        }
    }
    for (std::uint8_t index = 0; index < plan.subscriptionCount; ++index) {
        if (cancelled(cmd.session)) { doDisconnect(); return; }
        const auto& subscription = plan.subscriptions[index];
        NimBLERemoteCharacteristic* characteristic =
            findCached(client, subscription.service, subscription.characteristic);
        if (!characteristic) {
            if (subscription.ifPresent) continue;
            reportSubscription(index, subscription, ble::SubscriptionOutcome::NotFound);
            continue;
        }
        const bool supported = subscription.indicate ? characteristic->canIndicate() : characteristic->canNotify();
        if (!supported) {
            reportSubscription(index, subscription, ble::SubscriptionOutcome::Unsupported);
            continue;
        }
        // Runs on the NimBLE host task: queue only.
        auto callback = [this, session, index](NimBLERemoteCharacteristic*, std::uint8_t* data, std::size_t length,
                                               bool isNotify) {
#if defined(DEBUG_WEB)
            pushEvent(ble::CentralEventType::Notification, session, index,
                      isNotify ? 0U : ble::kSubscriptionIndicate, data, length);
#else
            (void)isNotify;
            pushEvent(ble::CentralEventType::Notification, session, index, 0, data, length);
#endif
        };
        progress(ble::ConnectPhase::Subscribe);
        const bool subscribed = characteristic->subscribe(!subscription.indicate, callback, true);
        if (cancelled(cmd.session)) { doDisconnect(); return; }
        if (subscribed) mask |= 1UL << index;
        reportSubscription(index, subscription,
                           subscribed ? ble::SubscriptionOutcome::Ok : ble::SubscriptionOutcome::CccdFailed);
    }
    // index = number of discovered characteristics that did not fit.
    if (overflowCount != 0) reportSubscription(overflowCount, firstOverflow, ble::SubscriptionOutcome::Overflow);
    if (plan.reportServices) {
        std::size_t reported = 0;
        for (auto* service : *services) {
            if (reported >= ble::kMaxAdvertisedServices) break;
            ble::Uuid uuid;
            if (!ble::Uuid::parse(service->getUUID().toString(), uuid) || genericService(uuid)) continue;
            pushEvent(ble::CentralEventType::GattService, session, 0, 0, uuid.bytes.data(), uuid.bytes.size());
            ++reported;
        }
    }
    std::lock_guard<std::mutex> lock(commandMutex_);
    if (cancelled(cmd.session)) {
        if (client->isConnected()) client->disconnect();
        return;
    }
    if (!client->isConnected()) {
        pushEvent(ble::CentralEventType::ConnectFailed, cmd.session, ble::kConnectFailure, 0, nullptr, 0,
                  static_cast<std::uint16_t>(client->getLastError()));
        return;
    }
    pendingConnection_.complete(cmd.session);
    pushEvent(ble::CentralEventType::Connected, cmd.session, 0, mask);
}

void NimbleCentral::doDisconnect() {
    std::lock_guard<std::mutex> lock(commandMutex_);
    if (impl_->client->isConnected()) impl_->client->disconnect();
}

void NimbleCentral::doWrite(const Cmd& cmd) {
    NimBLEClient* client = impl_->client;
    const auto failed = [this, &cmd]() {
#if defined(DEBUG_WEB)
        std::uint8_t endpoint[32];
        std::copy(cmd.write.service.bytes.begin(), cmd.write.service.bytes.end(), endpoint);
        std::copy(cmd.write.characteristic.bytes.begin(), cmd.write.characteristic.bytes.end(), endpoint + 16);
        pushEvent(ble::CentralEventType::WriteFailed, cmd.session, 0, 0, endpoint, sizeof(endpoint));
#else
        pushEvent(ble::CentralEventType::WriteFailed, cmd.session);
#endif
    };
    if (cmd.session != session_.load() || cancelled(cmd.session) || !client->isConnected()) {
        failed();
        return;
    }
    NimBLERemoteCharacteristic* characteristic =
        findCached(client, cmd.write.service, cmd.write.characteristic);
    if (!characteristic) {
        failed();
        return;
    }
    bool response = cmd.write.withResponse;
    // Vendor characteristics often only allow one of the two write types.
    if (response && !characteristic->canWrite() && characteristic->canWriteNoResponse()) response = false;
    if (!response && !characteristic->canWriteNoResponse() && characteristic->canWrite()) response = true;
    if (!characteristic->writeValue(cmd.write.data.data(), cmd.write.length, response))
        failed();
}

#endif  // ARDUINO
