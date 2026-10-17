#include "ble_input_adapter.h"

#include <algorithm>
#include <utility>

#ifdef ARDUINO
#include <Arduino.h>
#include "debug_serial.h"
#endif

#include "protocols/registry.hpp"
#include "scan_label.h"
#include "debug_log.h"

using namespace smartfitness;

namespace {

bool reached(std::uint32_t now, std::uint32_t deadline) { return static_cast<std::int32_t>(now - deadline) >= 0; }

bool validFtmsData(const ble::Advertisement& advertisement) {
    const auto& data = advertisement.ftmsServiceData;
    return data.size() == 3 && (data[0] & 1U) && (data[1] & 0x3fU);
}

#ifdef ARDUINO
const char* phaseName(ble::ConnectPhase phase) {
    switch (phase) {
        case ble::ConnectPhase::Connect: return "connect";
        case ble::ConnectPhase::Discovery: return "discovery";
        case ble::ConnectPhase::Read: return "read";
        case ble::ConnectPhase::Subscribe: return "subscribe";
    }
    return "unknown";
}
#endif

}  // namespace

BleInputAdapter::BleInputAdapter(ble::CentralTransport& transport, Clock clock)
    : BleInputAdapter(transport, std::move(clock), Timing{}) {}

BleInputAdapter::BleInputAdapter(ble::CentralTransport& transport, Clock clock, const Timing& timing)
    : transport_(transport), clock_(std::move(clock)), timing_(timing) {}

const char* BleInputAdapter::linkStateName(LinkState state) {
    switch (state) {
        case LinkState::Idle: return "idle";
        case LinkState::Probing: return "searching";
        case LinkState::Connecting: return "connecting";
        case LinkState::Ready: return "connected";
        case LinkState::Backoff: return "reconnecting";
        case LinkState::Listening: return "listening";
    }
    return "unknown";
}

void BleInputAdapter::begin() {
    if (autoConnect_ && paired_.valid && state_ == LinkState::Idle) startConnection();
}

bool BleInputAdapter::scanAndConnect() {
    if (!paired_.valid) {
        lastError_ = "no paired device";
        return false;
    }
    autoConnect_ = true;
    backoffMs_ = 0;
    attempts_ = 0;
    reconnecting_ = false;
    const bool closed = closeLink();
    state_ = LinkState::Idle;
    if (!closed) {
        maybeEmit(true);
        return false;
    }
    return startConnection();
}

void BleInputAdapter::disconnect() {
    requestDisconnect();
}

bool BleInputAdapter::requestDisconnect() {
    autoConnect_ = false;
    const bool accepted = closeLink();
    state_ = LinkState::Idle;
    backoffMs_ = 0;
    maybeEmit(true);
    return accepted;
}

void BleInputAdapter::setPairedDevice(const PairedDevice& device) {
    const bool changed = device.address != paired_.address || device.adapterId != paired_.adapterId ||
                         device.valid != paired_.valid;
    if (changed && state_ != LinkState::Idle) {
        closeLink();
        state_ = LinkState::Idle;
        maybeEmit(true);
    }
    auto cachedServices = changed ? device.advertisedServices : paired_.advertisedServices;
    paired_ = device;
    paired_.advertisedServices = std::move(cachedServices);
    if (changed) {
        protocol_.reset();
        advertisedServices_.clear();
        subscriptions_.clear();
        hasPairedAdvertisement_ = false;
        pairedAdvertisement_ = {};
        if (const auto* result = findResult(device.address)) {
            pairedAdvertisement_.address = result->address;
            pairedAdvertisement_.name = result->name;
            pairedAdvertisement_.rssi = result->rssi;
            pairedAdvertisement_.services = result->services;
            pairedAdvertisement_.ftmsServiceData = result->ftmsServiceData;
            pairedAdvertisementMs_ = result->lastSeenMs;
            pairedServiceDataMs_ = result->ftmsDataSeenMs;
            hasPairedAdvertisement_ = true;
        }
    }
}

void BleInputAdapter::clearPairedDevice() {
    disconnect();
    paired_ = {};
    hasPairedAdvertisement_ = false;
    pairedAdvertisement_ = {};
    protocol_.reset();
}

bool BleInputAdapter::ensureProtocol() {
    if (!protocol_ || paired_.adapterId != protocol_->id()) protocol_ = protocols::createProtocol(paired_.adapterId);
    return protocol_ != nullptr;
}

bool BleInputAdapter::startConnection() {
    const std::uint32_t now = clock_();
    if (!paired_.valid) {
        lastError_ = "no paired device";
        state_ = LinkState::Idle;
        return false;
    }
    if (isExcluded(paired_.address)) {
        lastError_ = "refusing to connect to the bridge itself or its FTMS client";
        state_ = LinkState::Idle;
        return false;
    }
    if (!ensureProtocol()) {
        lastError_ = "unknown adapter '" + paired_.adapterId + "'";
        state_ = LinkState::Idle;
        return false;
    }
    protocol_->reset();
    advertisedServices_ = paired_.advertisedServices;
    subscriptions_.clear();
    subscriptionIndices_.clear();
    subscriptionOptional_.clear();
    ++attempts_;
    stateSinceMs_ = now;
    if (!protocol_->advertisementOnly() &&
        (!hasPairedAdvertisement_ || now - pairedAdvertisementMs_ >= 2000)) {
        state_ = LinkState::Probing;
        probeFound_ = false;
        pairedAdvertisement_.ftmsServiceData.clear();
        if (!userScan_ && !transport_.startProbe(paired_.address, timing_.probeMs)) {
            scheduleReconnect("probe scan request rejected");
            return false;
        }
        return true;
    }
    return connectAfterProbe();
}

bool BleInputAdapter::connectAfterProbe() {
    const std::uint32_t now = clock_();
    if (hasPairedAdvertisement_ && now - pairedAdvertisementMs_ < 2000) {
        if (now - pairedServiceDataMs_ >= 2000) pairedAdvertisement_.ftmsServiceData.clear();
        for (const auto& service : pairedAdvertisement_.services) {
            if (advertisedServices_.size() < ble::kMaxAdvertisedServices &&
                std::find(advertisedServices_.begin(), advertisedServices_.end(), service) == advertisedServices_.end())
                advertisedServices_.push_back(service);
        }
        paired_.advertisedServices = advertisedServices_;
        protocol_->onAdvertisement(pairedAdvertisement_, now);
    }
    stateSinceMs_ = now;
    progressMs_ = now;
    connectPhase_ = ble::ConnectPhase::Connect;
    progressResults_ = 0;
#ifdef ARDUINO
    if (!reconnecting_ && attempts_ == 1) {
        if (paired_.name.empty())
            DBG_SERIAL.printf("[BLE] connecting to %s\n", paired_.address.toString().c_str());
        else
            DBG_SERIAL.printf("[BLE] connecting to %s (%s)\n", paired_.address.toString().c_str(),
                              paired_.name.c_str());
    } else if (!hasReconnectLog_ || now - lastReconnectLogMs_ >= 10000) {
        DBG_SERIAL.printf("[BLE] reconnect attempt %u to %s\n", static_cast<unsigned>(attempts_),
                          paired_.address.toString().c_str());
        lastReconnectLogMs_ = now;
        hasReconnectLog_ = true;
    }
    if (hasPairedAdvertisement_ && now - pairedAdvertisementMs_ < 2000) {
        static const char* const names[] = {
            "treadmill", "cross-trainer", "step-climber", "stair-climber", "rower", "indoor-bike"
        };
        std::string types;
        if (validFtmsData(pairedAdvertisement_)) {
            for (std::size_t index = 0; index < 6; ++index) {
                if (!(pairedAdvertisement_.ftmsServiceData[1] & (1U << index))) continue;
                if (!types.empty()) types += ",";
                types += names[index];
            }
        }
        DBG_SERIAL.printf("[BLE] advertisement: rssi=%d, ftms types=%s\n",
                          pairedAdvertisement_.rssi, types.empty() ? "none" : types.c_str());
    }
#endif
    if (protocol_->advertisementOnly()) {
        state_ = LinkState::Listening;
        if (!transport_.startScan(0, true)) {
            state_ = LinkState::Idle;
            scheduleReconnect("listening scan request rejected");
            return false;
        }
#ifdef ARDUINO
        DBG_SERIAL.println("[BLE] scan started for broadcast adapter");
#endif
        return true;
    }
    if ((userScan_ || transport_.scanning()) && !transport_.stopScan()) {
        scheduleReconnect("stop scan request rejected");
        return false;
    }
    userScan_ = false;
    ++session_;
    auto plan = protocol_->plan();
#if defined(ARDUINO) && defined(DEBUG_WEB)
    debugReads_.assign(plan.reads.begin(), plan.reads.begin() + plan.readCount);
    debugConnectMs_ = now;
    debugSequence_ = debugNotifications_ = 0;
    debugStatsMs_ = now;
#endif
    // Discovery plans start empty; the central reports what it subscribed.
    for (std::size_t index = 0; index < plan.subscriptionCount && index < ble::kMaxSubscriptions; ++index) {
        const auto& subscription = plan.subscriptions[index];
        subscriptions_.push_back({subscription.service, subscription.characteristic, subscription.indicate, "pending", ""});
        subscriptionIndices_.push_back(static_cast<std::uint8_t>(index));
        subscriptionOptional_.push_back(subscription.ifPresent);
    }
    expectedResults_ = plan.readCount + plan.subscriptionCount;
    plan.reportServices = advertisedServices_.empty();
    gattServicesRequested_ = plan.reportServices;
    gattServices_.clear();
    if (!transport_.connect(session_, paired_.address, plan)) {
        scheduleReconnect("connect request rejected", "connect-failed");
        return false;
    }
    state_ = LinkState::Connecting;
    return true;
}

bool BleInputAdapter::closeLink() {
    advertisedServices_.clear();
    subscriptions_.clear();
    subscriptionIndices_.clear();
    subscriptionOptional_.clear();
    bool accepted = true;
    if (state_ == LinkState::Connecting || state_ == LinkState::Ready) {
        accepted = transport_.disconnect(session_);
        // Idle/Backoff ignore this session; the next attempt gets a new ID.
    }
    if ((state_ == LinkState::Listening || state_ == LinkState::Probing) && !userScan_)
        accepted = transport_.stopScan() && accepted;
    if (protocol_) protocol_->reset();
    targetPower_ = 0.0F;
    hasTargetPower_ = false;
    if (!accepted) lastError_ = "disconnect/stop request rejected";
    return accepted;
}

void BleInputAdapter::scheduleReconnect(const std::string& reason, const char* subscriptionReason) {
    lastError_ = reason;
    std::vector<BleSubscriptionStatus> kept;
    auto keptIndices = subscriptionIndices_;
    auto keptOptional = subscriptionOptional_;
    if (subscriptionReason) {
        for (auto& subscription : subscriptions_) {
            if (subscription.state != "pending") continue;
            subscription.state = "failed";
            subscription.reason = subscriptionReason;
        }
        kept.swap(subscriptions_);
    }
    closeLink();
    subscriptions_ = std::move(kept);
    if (subscriptionReason) {
        subscriptionIndices_ = std::move(keptIndices);
        subscriptionOptional_ = std::move(keptOptional);
    }
    if (autoConnect_ && paired_.valid) {
        reconnecting_ = true;
        backoffMs_ = backoffMs_ == 0 ? timing_.backoffInitialMs : backoffMs_ * 2;
        if (backoffMs_ > timing_.backoffMaxMs) backoffMs_ = timing_.backoffMaxMs;
        nextAttemptMs_ = clock_() + backoffMs_;
        state_ = LinkState::Backoff;
    } else {
        state_ = LinkState::Idle;
    }
    maybeEmit(true);
}

std::uint32_t BleInputAdapter::nextAttemptInMs() const {
    if (state_ != LinkState::Backoff) return 0;
    const std::uint32_t now = clock_();
    return reached(now, nextAttemptMs_) ? 0 : nextAttemptMs_ - now;
}

void BleInputAdapter::connectTimedOut(std::uint32_t now) {
#ifdef ARDUINO
    DBG_SERIAL.printf("[BLE] connect timeout after %u ms (phase=%s, results=%u/%u)\n",
                      static_cast<unsigned>(now - stateSinceMs_), phaseName(connectPhase_),
                      static_cast<unsigned>(progressResults_), static_cast<unsigned>(expectedResults_));
#else
    (void)now;
#endif
    scheduleReconnect("connect timeout", "timeout");
}

void BleInputAdapter::loop() {
    ble::ScanReport report;
    while (transport_.pollScan(report)) handleScanReport(report);
    if (state_ == LinkState::Connecting && clock_() - stateSinceMs_ >= timing_.connectMaxMs)
        connectTimedOut(clock_());
    ble::CentralEvent event;
    while (transport_.pollEvent(event)) handleEvent(event);
    const std::uint32_t lifecycleDrops = transport_.droppedLifecycleEvents();
    if (lifecycleDrops != observedLifecycleDrops_) {
        observedLifecycleDrops_ = lifecycleDrops;
        lastError_ = "BLE lifecycle event overflow";
        if (state_ == LinkState::Connecting || state_ == LinkState::Ready)
            scheduleReconnect(lastError_, state_ == LinkState::Connecting ? "event-overflow" : nullptr);
        if (!transport_.scanning()) userScan_ = false;
    }

    const std::uint32_t now = clock_();
    if (userScan_ && reached(now, scanUntilMs_)) {
        if (state_ == LinkState::Probing) userScan_ = false;
        else stopScan();
    }

    switch (state_) {
        case LinkState::Probing:
            if (probeFound_ && (validFtmsData(pairedAdvertisement_) || now - probeFoundMs_ >= 500 ||
                                now - stateSinceMs_ >= timing_.probeMs)) {
                connectAfterProbe();
            } else if (now - stateSinceMs_ >= timing_.probeMs) {
#ifdef ARDUINO
                DBG_SERIAL.printf("[BLE] %s not advertising (asleep, out of range or connected to another app)\n",
                                  paired_.address.toString().c_str());
#endif
                scheduleReconnect("not-advertising");
            } else if (!transport_.scanning() &&
                       !transport_.startProbe(paired_.address, timing_.probeMs - (now - stateSinceMs_))) {
                scheduleReconnect("probe scan request rejected");
            }
            break;
        case LinkState::Connecting:
            if (now - progressMs_ >= timing_.connectTimeoutMs || now - stateSinceMs_ >= timing_.connectMaxMs) {
                connectTimedOut(now);
            }
            break;
        case LinkState::Ready:
#if defined(ARDUINO) && defined(DEBUG_WEB)
            if (now - debugStatsMs_ >= 5000) {
                SF_DEBUG_LOG("[BLE-IN] stats: notifications=%u/5s, dropped(events=%u, notifications=%u, lifecycle=%u), lastData=%u ms ago, stale=%u\n",
                             static_cast<unsigned>(debugNotifications_), static_cast<unsigned>(droppedEvents()),
                             static_cast<unsigned>(droppedNotifications()), static_cast<unsigned>(droppedLifecycleEvents()),
                             static_cast<unsigned>(now - (protocol_->hasData() ? protocol_->lastDataMs() : stateSinceMs_)),
                             stale() ? 1U : 0U);
                debugNotifications_ = 0;
                debugStatsMs_ = now;
            }
#endif
            protocol_->tick(now);
            if (protocol_->failed()) scheduleReconnect(protocol_->lastError());
            else if (!protocol_->hasData()) {
                const std::uint32_t grace = protocol_->startupTimeoutMs();
                const std::uint32_t boundedGrace = grace > 120000U ? 120000U : grace;
                const std::uint32_t timeout = boundedGrace > timing_.noDataReconnectMs
                    ? boundedGrace : timing_.noDataReconnectMs;
                if (now - stateSinceMs_ > timeout) scheduleReconnect("no data from device");
            }
            break;
        case LinkState::Backoff:
            if (reached(now, nextAttemptMs_)) startConnection();
            break;
        case LinkState::Listening:
            protocol_->tick(now);
            if (!transport_.scanning() && !transport_.startScan(0, true))
                scheduleReconnect("listening scan request rejected");
            break;
        case LinkState::Idle: break;
    }
    flushWrites();
    maybeEmit(false);
}

void BleInputAdapter::handleEvent(const ble::CentralEvent& event) {
    const std::uint32_t now = clock_();
#if defined(ARDUINO) && defined(DEBUG_WEB)
    logInputEvent(event, now);
#endif
    if (event.type == ble::CentralEventType::ScanComplete) {
        // A stop's completion can remain queued while a new scan is requested.
#ifdef ARDUINO
        if (userScan_)
            DBG_SERIAL.printf("[BLE] scan finished: %u device(s)\n", static_cast<unsigned>(results_.size()));
#endif
        if (!transport_.scanning()) userScan_ = false;
        return;
    }
    if (event.session != session_ || !protocol_) return;
    const bool linkActive = state_ == LinkState::Connecting || state_ == LinkState::Ready;
    if (state_ == LinkState::Connecting) {
        switch (event.type) {
            case ble::CentralEventType::ReadResult:
                connectPhase_ = ble::ConnectPhase::Read;
                ++progressResults_;
                progressMs_ = now;
                break;
            case ble::CentralEventType::SubscriptionResult:
                connectPhase_ = ble::ConnectPhase::Subscribe;
                ++progressResults_;
                progressMs_ = now;
                break;
            case ble::CentralEventType::GattService:
            case ble::CentralEventType::WriteEndpoint:
                connectPhase_ = ble::ConnectPhase::Discovery;
                progressMs_ = now;
                break;
            case ble::CentralEventType::ConnectProgress:
                if (event.index <= static_cast<std::uint8_t>(ble::ConnectPhase::Subscribe)) {
                    connectPhase_ = static_cast<ble::ConnectPhase>(event.index);
                    progressMs_ = now;
                }
                break;
            default: break;
        }
    }
    switch (event.type) {
        case ble::CentralEventType::ReadResult:
            if (linkActive) protocol_->onRead(event.index, event.data.data(), event.length);
            break;
        case ble::CentralEventType::WriteEndpoint:
            if (linkActive && event.length == 32) {
                ble::Uuid service, characteristic;
                for (std::size_t index = 0; index < service.bytes.size(); ++index) {
                    service.bytes[index] = event.data[index];
                    characteristic.bytes[index] = event.data[index + 16];
                }
                protocol_->onWriteEndpoint(service, characteristic);
            }
            break;
        case ble::CentralEventType::SubscriptionResult:
            if (state_ == LinkState::Connecting && event.length == 32) applySubscriptionResult(event);
            break;
        case ble::CentralEventType::GattService:
            if (state_ == LinkState::Connecting && gattServicesRequested_ && event.length == 16 &&
                gattServices_.size() < ble::kMaxAdvertisedServices) {
                ble::Uuid service;
                std::copy(event.data.begin(), event.data.begin() + 16, service.bytes.begin());
                if (std::find(gattServices_.begin(), gattServices_.end(), service) == gattServices_.end())
                    gattServices_.push_back(service);
            }
            break;
        case ble::CentralEventType::Connected:
            if (state_ != LinkState::Connecting) break;
            // Fallback for results lost in the event queue: derive from the mask.
            for (std::size_t index = 0; index < subscriptions_.size() && index < ble::kMaxSubscriptions; ++index) {
                auto& subscription = subscriptions_[index];
                if (subscription.state != "pending") continue;
                const bool subscribed = (event.mask & (1U << subscriptionIndices_[index])) != 0;
                if (subscriptionOptional_[index] && !subscribed) continue;
                subscription.state = subscribed ? "ok" : "failed";
#ifdef ARDUINO
                DBG_SERIAL.printf("[BLE] subscribe %s/%s %s: %s\n", subscription.service.toString().c_str(),
                                  subscription.characteristic.toString().c_str(),
                                  subscription.indicate ? "indicate" : "notify", subscribed ? "ok" : "no result");
#endif
            }
            for (std::size_t index = subscriptions_.size(); index-- > 0;) {
                if (subscriptionOptional_[index] &&
                    (subscriptions_[index].state == "pending" || subscriptions_[index].reason == "not-found")) {
                    subscriptions_.erase(subscriptions_.begin() + index);
                    subscriptionIndices_.erase(subscriptionIndices_.begin() + index);
                    subscriptionOptional_.erase(subscriptionOptional_.begin() + index);
                }
            }
            if (protocol_->onConnected(event.mask, now)) {
                if (advertisedServices_.empty() && !gattServices_.empty()) {
                    advertisedServices_ = gattServices_;
                    paired_.advertisedServices = advertisedServices_;
#ifdef ARDUINO
                    std::string list;
                    for (const auto& service : advertisedServices_) list += " " + service.toString();
                    DBG_SERIAL.printf("[BLE] GATT services:%s\n", list.c_str());
#endif
                }
                state_ = LinkState::Ready;
                stateSinceMs_ = now;
#if defined(ARDUINO) && defined(DEBUG_WEB)
                debugConnectMs_ = debugStatsMs_ = now;
#endif
                backoffMs_ = 0;
                attempts_ = 0;
                reconnecting_ = false;
                lastError_.clear();
#ifdef ARDUINO
                DBG_SERIAL.printf("[BLE] connected; adapter=%s, notifications=%u/%u\n", protocol_->id(),
                                  static_cast<unsigned>(std::count_if(subscriptions_.begin(), subscriptions_.end(),
                                      [](const BleSubscriptionStatus& status) { return status.state == "ok"; })),
                                  static_cast<unsigned>(subscriptions_.size()));
#endif
                maybeEmit(true);
            } else {
                scheduleReconnect(protocol_->lastError().empty() ? "device not supported" : protocol_->lastError(),
                                  "rejected");
            }
            break;
        case ble::CentralEventType::ConnectFailed:
            if (linkActive) {
#ifdef ARDUINO
                if (event.index == ble::kServiceDiscoveryFailure)
                    DBG_SERIAL.printf("[BLE] service discovery failed (reason=%u)\n", event.reasonCode);
                else
                    DBG_SERIAL.printf("[BLE] connection failed (reason=%u)\n", event.reasonCode);
#endif
                const char* subscriptionReason =
                    event.index == ble::kServiceDiscoveryFailure ? "discovery-failed" : "connect-failed";
                scheduleReconnect("connect failed", state_ == LinkState::Connecting ? subscriptionReason : nullptr);
            }
            break;
        case ble::CentralEventType::Disconnected:
            if (linkActive) {
#ifdef ARDUINO
                DBG_SERIAL.printf("[BLE] disconnected (reason=%u)\n", event.reasonCode);
#endif
                scheduleReconnect("link lost", state_ == LinkState::Connecting ? "disconnected" : nullptr);
            }
            break;
        case ble::CentralEventType::Notification:
            if (linkActive) protocol_->onNotify(event.index, event.data.data(), event.length, now);
            break;
        case ble::CentralEventType::WriteFailed:
            if (linkActive) {
                protocol_->onWriteFailed(now);
                lastError_ = protocol_->lastError();
            }
            break;
        case ble::CentralEventType::ScanComplete: break;
        case ble::CentralEventType::ConnectProgress: break;
    }
}

#if defined(ARDUINO) && defined(DEBUG_WEB)
void BleInputAdapter::logInputEvent(const ble::CentralEvent& event, std::uint32_t now) {
    if (event.type != ble::CentralEventType::Notification && event.type != ble::CentralEventType::ReadResult &&
        event.type != ble::CentralEventType::WriteFailed) return;
    const char* ignored = event.session != session_ ? "session mismatch" :
                          !protocol_ ? "no protocol" :
                          state_ != LinkState::Connecting && state_ != LinkState::Ready ? "link inactive" : nullptr;
    // Old sessions must not be labelled with this connection's UUIDs.
    char endpoint[80];
    std::snprintf(endpoint, sizeof(endpoint), "idx=%u", event.index);
    const BleSubscriptionStatus* subscription = nullptr;
    if (event.session == session_) {
        if (event.type == ble::CentralEventType::Notification) {
            const auto found = std::find(subscriptionIndices_.begin(), subscriptionIndices_.end(), event.index);
            if (found != subscriptionIndices_.end()) {
                subscription = &subscriptions_[found - subscriptionIndices_.begin()];
                std::snprintf(endpoint, sizeof(endpoint), "%s/%s", subscription->service.toString().c_str(),
                              subscription->characteristic.toString().c_str());
            }
        } else if (event.type == ble::CentralEventType::ReadResult && event.index < debugReads_.size()) {
            const auto& read = debugReads_[event.index];
            std::snprintf(endpoint, sizeof(endpoint), "%s/%s", read.service.toString().c_str(),
                          read.characteristic.toString().c_str());
        }
    }
    const auto length = event.originalLength ? event.originalLength : event.length;
    const debuglog::Hex hex(event.data.data(), event.length, length);
    const char* suffix = ignored ? " (ignored: " : "";
    const char* reason = ignored ? ignored : "";
    const char* end = ignored ? ")" : "";
    if (event.type == ble::CentralEventType::Notification) {
        ++debugSequence_;
        if (event.session == session_) ++debugNotifications_;
        SF_DEBUG_LOG("[BLE-IN] #%u +%u %s %s len=%zu: %s%s%s%s\n",
                     static_cast<unsigned>(debugSequence_), static_cast<unsigned>(now - debugConnectMs_), endpoint,
                     subscription ? (subscription->indicate ? "indicate" : "notify") :
                                    ((event.mask & ble::kSubscriptionIndicate) ? "indicate" : "notify"),
                     length, hex.c_str(), suffix, reason, end);
    } else if (event.type == ble::CentralEventType::ReadResult) {
        SF_DEBUG_LOG("[BLE-READ] %s len=%zu: %s%s%s%s\n", endpoint, length, hex.c_str(), suffix, reason, end);
    } else {
        if (event.length == 32) {
            ble::Uuid service, characteristic;
            std::copy_n(event.data.begin(), 16, service.bytes.begin());
            std::copy_n(event.data.begin() + 16, 16, characteristic.bytes.begin());
            std::snprintf(endpoint, sizeof(endpoint), "%s/%s", service.toString().c_str(),
                          characteristic.toString().c_str());
        }
        SF_DEBUG_LOG("[BLE-OUT] %s WriteFailed reason=%u%s%s%s\n", endpoint, event.reasonCode, suffix, reason, end);
    }
}
#endif

void BleInputAdapter::applySubscriptionResult(const ble::CentralEvent& event) {
    BleSubscriptionStatus status;
    std::copy(event.data.begin(), event.data.begin() + 16, status.service.bytes.begin());
    std::copy(event.data.begin() + 16, event.data.begin() + 32, status.characteristic.bytes.begin());
    status.indicate = (event.mask & ble::kSubscriptionIndicate) != 0;
    const auto outcome = static_cast<ble::SubscriptionOutcome>(event.reasonCode);
    auto found = std::find(subscriptionIndices_.begin(), subscriptionIndices_.end(), event.index);
    if (outcome != ble::SubscriptionOutcome::Overflow && found != subscriptionIndices_.end() &&
        subscriptionOptional_[found - subscriptionIndices_.begin()] && outcome == ble::SubscriptionOutcome::NotFound)
        return;
    status.state = outcome == ble::SubscriptionOutcome::Ok ? "ok" : "failed";
    if (outcome != ble::SubscriptionOutcome::Ok) status.reason = ble::subscriptionOutcomeName(outcome);
#ifdef ARDUINO
    if (outcome == ble::SubscriptionOutcome::Overflow)
        DBG_SERIAL.printf("[BLE] subscribe %s/%s %s: overflow (%u characteristic(s) beyond limit %u)\n",
                          status.service.toString().c_str(), status.characteristic.toString().c_str(),
                          status.indicate ? "indicate" : "notify", static_cast<unsigned>(event.index),
                          static_cast<unsigned>(ble::kMaxSubscriptions));
    else
        DBG_SERIAL.printf("[BLE] subscribe %s/%s %s: %s\n", status.service.toString().c_str(),
                          status.characteristic.toString().c_str(), status.indicate ? "indicate" : "notify",
                          ble::subscriptionOutcomeName(outcome));
#endif
    if (outcome == ble::SubscriptionOutcome::Overflow) {
        // One entry stands for every discovered characteristic that did not fit.
        subscriptions_.push_back(std::move(status));
        subscriptionIndices_.push_back(ble::kMaxSubscriptions);
        subscriptionOptional_.push_back(false);
        return;
    }
    if (event.index >= ble::kMaxSubscriptions) return;
    if (found == subscriptionIndices_.end()) {
        subscriptions_.push_back(std::move(status));
        subscriptionIndices_.push_back(event.index);
        subscriptionOptional_.push_back(false);
    } else {
        subscriptions_[found - subscriptionIndices_.begin()] = std::move(status);
    }
}

bool BleInputAdapter::startScan(std::uint32_t durationMs) {
    if (state_ == LinkState::Connecting) {
        lastError_ = "busy connecting";
        return false;
    }
    if (durationMs == 0) durationMs = kDefaultScanMs;
    results_.clear();
    droppedResults_ = 0;
    userScan_ = true;
    scanUntilMs_ = clock_() + durationMs;
    // While listening to a broadcast device the continuous scan is reused.
    if (state_ == LinkState::Listening) {
#ifdef ARDUINO
        DBG_SERIAL.println("[BLE] scan started (reusing broadcast scan)");
#endif
        return true;
    }
    if (!transport_.startScan(durationMs, true, true)) {
        userScan_ = false;
        lastError_ = "scan could not be started";
        return false;
    }
#ifdef ARDUINO
    DBG_SERIAL.printf("[BLE] scan started for %u s\n", static_cast<unsigned>((durationMs + 999) / 1000));
#endif
    return true;
}

bool BleInputAdapter::stopScan() {
    if (state_ == LinkState::Probing && userScan_) {
        const bool cancelled = requestDisconnect();
        if (!transport_.stopScan()) {
            lastError_ = "stop scan request rejected";
            return false;
        }
        userScan_ = false;
        return cancelled;
    }
    if (state_ == LinkState::Connecting || state_ == LinkState::Probing) return requestDisconnect();
    if (!userScan_) return true;
    if (state_ != LinkState::Listening && !transport_.stopScan()) {
        lastError_ = "stop scan request rejected";
        return false;
    }
    userScan_ = false;
#ifdef ARDUINO
    DBG_SERIAL.printf("[BLE] scan stopped: %u device(s)\n", static_cast<unsigned>(results_.size()));
#endif
    return true;
}

const ScanResult* BleInputAdapter::findResult(const ble::BleAddress& address) const {
    for (const auto& result : results_) {
        if (result.address == address) return &result;
    }
    return nullptr;
}

void BleInputAdapter::setExcludedPeer(const ble::BleAddress& address, bool valid) {
    excludedPeer_ = address;
    excludedPeerValid_ = valid;
}

bool BleInputAdapter::isExcluded(const ble::BleAddress& address) const {
    const ble::BleAddress own = transport_.ownAddress();
    if (!own.isZero() && own == address) return true;
    return excludedPeerValid_ && excludedPeer_ == address;
}

void BleInputAdapter::handleScanReport(const ble::ScanReport& report) {
    if (isExcluded(report.address)) return;
    const std::uint32_t now = clock_();
    ble::Advertisement advertisement = ble::Advertisement::from(report);
    if (paired_.valid && report.address == paired_.address) {
        if (!hasPairedAdvertisement_ || now - pairedAdvertisementMs_ >= 2000 ||
            (state_ == LinkState::Probing && !probeFound_)) {
            pairedAdvertisement_ = advertisement;
        } else {
            pairedAdvertisement_.address = advertisement.address;
            pairedAdvertisement_.rssi = advertisement.rssi;
            if (!advertisement.name.empty()) pairedAdvertisement_.name = advertisement.name;
            if (!advertisement.ftmsServiceData.empty()) pairedAdvertisement_.ftmsServiceData = advertisement.ftmsServiceData;
            for (const auto& service : advertisement.services)
                if (!pairedAdvertisement_.hasService(service) && pairedAdvertisement_.services.size() < ble::kMaxAdvertisedServices)
                    pairedAdvertisement_.services.push_back(service);
        }
        pairedAdvertisementMs_ = now;
        if (!advertisement.ftmsServiceData.empty()) pairedServiceDataMs_ = now;
        else if (now - pairedServiceDataMs_ >= 2000) pairedAdvertisement_.ftmsServiceData.clear();
        hasPairedAdvertisement_ = true;
        if (state_ == LinkState::Probing && !probeFound_) {
            probeFound_ = true;
            probeFoundMs_ = now;
        }
        for (const auto& service : advertisement.services) {
            auto& cached = paired_.advertisedServices;
            if (cached.size() < ble::kMaxAdvertisedServices &&
                std::find(cached.begin(), cached.end(), service) == cached.end())
                cached.push_back(service);
        }
        if (state_ == LinkState::Listening || state_ == LinkState::Connecting || state_ == LinkState::Ready)
            advertisedServices_ = paired_.advertisedServices;
    }
    if (protocol_ && report.address == paired_.address &&
        (state_ == LinkState::Listening || state_ == LinkState::Connecting || state_ == LinkState::Ready))
        protocol_->onAdvertisement(advertisement, now);
    if (!userScan_) return;

    ScanResult* entry = nullptr;
    for (auto& result : results_) {
        if (result.address == report.address) entry = &result;
    }
    if (!entry) {
        if (results_.size() >= kMaxScanResults) {
            // A full list keeps fitness devices: a relevant newcomer replaces
            // the weakest entry without name, known service or protocol.
            ScanResult* weakest = nullptr;
            if (isRelevantDevice(advertisement.name, protocols::detectProtocol(advertisement) ? "detected" : "",
                                 advertisement.services)) {
                for (auto& result : results_) {
                    if (isRelevantDevice(result.name, result.detectedAdapter, result.services)) continue;
                    if (!weakest || result.rssi < weakest->rssi) weakest = &result;
                }
            }
            ++droppedResults_;
            if (!weakest) return;
            *weakest = ScanResult{};
            entry = weakest;
        } else {
            results_.emplace_back();
            entry = &results_.back();
        }
        entry->address = report.address;
        entry->connectable = report.connectable;
    } else {
        entry->connectable = entry->connectable || report.connectable;
    }
    entry->address.type = report.address.type;
    entry->rssi = report.rssi;
    if (now - entry->ftmsDataSeenMs >= 2000) entry->ftmsServiceData.clear();
    entry->lastSeenMs = now;
    if (!advertisement.name.empty()) entry->name = advertisement.name;
    if (!advertisement.ftmsServiceData.empty()) {
        entry->ftmsServiceData = advertisement.ftmsServiceData;
        entry->ftmsDataSeenMs = now;
    }
    if (!advertisement.manufacturerData.empty()) entry->manufacturerData = advertisement.manufacturerData;
    for (const auto& service : advertisement.services) {
        bool known = false;
        for (const auto& existing : entry->services) known = known || existing == service;
        if (!known) entry->services.push_back(service);
    }
    // Detect on the merged view (name and services may arrive in different
    // packets); keep an earlier detection when this packet carries less.
    advertisement.name = entry->name;
    advertisement.services = entry->services;
    advertisement.ftmsServiceData = entry->ftmsServiceData;
    advertisement.manufacturerData = entry->manufacturerData;
    if (const auto* info = protocols::detectProtocol(advertisement)) entry->detectedAdapter = info->id;
}

bool BleInputAdapter::flushWrites() {
    if (!protocol_ || state_ != LinkState::Ready) return true;
    bool accepted = true;
    ble::GattWrite write;
    while (protocol_->takeWrite(write)) {
        SF_DEBUG_LOG("[BLE-OUT] %s/%s %s response len=%u: %s\n", write.service.toString().c_str(),
                     write.characteristic.toString().c_str(), write.withResponse ? "with" : "without",
                     write.length, debuglog::Hex(write.data.data(), write.length).c_str());
        if (!transport_.write(session_, write)) {
            SF_DEBUG_LOG("[BLE-OUT] %s/%s WriteFailed: write request rejected\n",
                         write.service.toString().c_str(), write.characteristic.toString().c_str());
            protocol_->onWriteFailed(clock_());
            lastError_ = "write request rejected";
            accepted = false;
        }
    }
    return accepted;
}

bool BleInputAdapter::isConnected() const {
    if (!protocol_) return false;
    if (state_ == LinkState::Ready) return true;
    if (state_ == LinkState::Listening)
        return protocol_->hasData() && clock_() - protocol_->lastDataMs() < timing_.noDataReconnectMs;
    return false;
}

bool BleInputAdapter::stale() const {
    if (!protocol_ || (state_ != LinkState::Ready && state_ != LinkState::Listening)) return false;
    const std::uint32_t now = clock_();
    if (!protocol_->hasData()) return now - stateSinceMs_ > timing_.staleMs;
    return now - protocol_->lastDataMs() > timing_.staleMs;
}

TrainerCapabilities BleInputAdapter::capabilities() const {
    if (!isConnected()) return {};
    TrainerCapabilities capabilities = protocol_->capabilities();
    // Stale telemetry: samples become invalid (and BridgeCore clears targets).
    if (stale()) {
        capabilities.metrics = 0;
        if (profile() == MachineProfile::Treadmill) capabilities.controls = 0;
    }
    return capabilities;
}

bool BleInputAdapter::trainerDetected() const { return isConnected() && capabilities().controls != 0; }

MachineProfile BleInputAdapter::profile() const {
    return protocol_ ? protocol_->profile() : MachineProfile::IndoorBike;
}

const char* BleInputAdapter::protocolId() const { return protocol_ ? protocol_->id() : ""; }

FitnessTelemetry BleInputAdapter::telemetry() const {
    FitnessTelemetry telemetry;
    telemetry.connected = isConnected();
    telemetry.trainerDetected = trainerDetected();
    telemetry.deviceName = paired_.name.empty() ? paired_.address.toString() : paired_.name;
    telemetry.deviceType = protocolId();
    if (!telemetry.connected) return telemetry;
    if (hasTargetPower_) telemetry.targetPower = targetPower_;
    // Stale data invalidates every metric (capabilities() clears them too).
    if (stale()) return telemetry;
    const auto& sample = protocol_->sample();
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        const auto metric = static_cast<Metric>(index);
        const auto& value = sample[metric];
        if (value.valid) telemetry.metric(metric) = value.value;
    }
    return telemetry;
}

bool BleInputAdapter::apply(const ControlRequest& request) {
    if (state_ != LinkState::Ready || !protocol_) return false;
    if (profile() == MachineProfile::Treadmill && stale()) return false;
    if (!protocol_->control(request, clock_())) return false;
    if (!flushWrites()) return false;
    if (request.control == Control::Power) {
        targetPower_ = request.value;
        hasTargetPower_ = true;
    }
    maybeEmit(true);
    return true;
}

void BleInputAdapter::maybeEmit(bool force) {
    const std::uint32_t now = clock_();
    const bool connected = isConnected();
    const bool isStale = stale();
    if (connected && isStale != lastEmittedStale_)
        SF_DEBUG_LOG("[BLE-IN] data %s\n", isStale ? "stale" : "fresh");
    const TrainerCapabilities caps = capabilities();
    const bool capsChanged = caps.metrics != lastEmittedCaps_.metrics || caps.controls != lastEmittedCaps_.controls;
    const bool fresh = connected && protocol_ && protocol_->hasData() &&
                       static_cast<std::int32_t>(protocol_->lastDataMs() - lastEmitMs_) > 0 &&
                       now - lastEmitMs_ >= timing_.telemetryIntervalMs;
    if (!force && !fresh && !capsChanged && connected == lastEmittedConnected_ && isStale == lastEmittedStale_)
        return;
    lastEmitMs_ = now;
    lastEmittedCaps_ = caps;
    lastEmittedConnected_ = connected;
    lastEmittedStale_ = isStale;
    emitTelemetry(telemetry());
}

InputSelector::InputSelector(SimulatedDeviceAdapter& simulation, BtDeviceAdapter* ble)
    : simulation_(simulation), ble_(ble) {
    simulation_.onTelemetry([this](const FitnessTelemetry& telemetry) {
        if (source_ == Source::Simulation) emitTelemetry(telemetry);
    });
    if (ble_) {
        ble_->onTelemetry([this](const FitnessTelemetry& telemetry) {
            if (source_ == Source::Ble) emitTelemetry(telemetry);
        });
    }
}

InputSelector::~InputSelector() {
    simulation_.onTelemetry({});
    if (ble_) ble_->onTelemetry({});
}

BtDeviceAdapter& InputSelector::active() {
    if (source_ == Source::Ble && ble_) return *ble_;
    return simulation_;
}

const BtDeviceAdapter& InputSelector::active() const {
    if (source_ == Source::Ble && ble_) return *ble_;
    return simulation_;
}

void InputSelector::select(Source source) {
    if (source == Source::Ble && !ble_) return;
    if (source == source_) return;
    active().disconnect();
    source_ = source;
    emitTelemetry(active().telemetry());
}

void InputSelector::begin() {
    simulation_.begin();
    if (ble_) ble_->begin();
}

void InputSelector::loop() {
    simulation_.loop();
    if (ble_) ble_->loop();
}

bool InputSelector::scanAndConnect() { return active().scanAndConnect(); }
void InputSelector::disconnect() { active().disconnect(); }
bool InputSelector::isConnected() const { return active().isConnected(); }
bool InputSelector::trainerDetected() const { return active().trainerDetected(); }
MachineProfile InputSelector::profile() const { return active().profile(); }
FitnessTelemetry InputSelector::telemetry() const { return active().telemetry(); }
TrainerCapabilities InputSelector::capabilities() const { return active().capabilities(); }
bool InputSelector::apply(const ControlRequest& request) { return active().apply(request); }
