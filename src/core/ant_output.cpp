#include "ant_output.h"
#include "ant/debug_log.hpp"

#include <chrono>
#include <utility>

#ifdef ARDUINO
#include <Arduino.h>
#include "debug_serial.h"
#endif

using namespace smartfitness;
using namespace smartfitness::ant;

// ANT+ managed network key (thisisant.com, ANT+ adopter terms apply).
// Override with -DSF_ANT_NETWORK_KEY=0x..,0x.. (8 bytes) for private networks.
#ifndef SF_ANT_NETWORK_KEY
#define SF_ANT_NETWORK_KEY 0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
#endif

namespace {
constexpr std::uint8_t kNetworkKey[8] = {SF_ANT_NETWORK_KEY};
constexpr std::size_t kSteps = 5;
constexpr std::uint8_t kRequestMessage = 0x4D;
constexpr std::uint8_t kCapabilities = 0x54;
constexpr std::uint8_t kMaxRequestRepeats = 8;
constexpr std::uint8_t kEventMessageId = 0x01;

bool reached(std::uint32_t now, std::uint32_t at) { return static_cast<std::int32_t>(now - at) >= 0; }
}  // namespace

void AntOutput::setTransport(AntTransport* transport) {
    if (transport_ != transport) closeChannel();
    transport_ = transport;
    parser_.reset();
    link_ = Link::WaitingForStick;
    openMask_ = 0;
    retryPending_ = false;
    error_ = Error::None;
}

void AntOutput::setDeviceNumber(std::uint16_t number) { deviceNumber_ = number ? number : 1; }

void AntOutput::setProfileOverride(FtmsOutputProfile profile) { profileOverride_ = profile; }

MachineProfile AntOutput::profile() const { return effectiveOutputProfile(profileOverride_, trainerState().profile); }

void AntOutput::setServices(std::uint8_t rawMask) {
    if (configuredServices_ == rawMask) return;
    closeChannel();
    configuredServices_ = rawMask;
    openMask_ = 0;
    retryPending_ = false;
    link_ = Link::WaitingForStick;
}

std::uint8_t AntOutput::services() const {
    std::uint8_t mask = configuredServices_ == 0 ? kFec : configuredServices_ & 31;
    if (profile() != MachineProfile::Treadmill) mask &= ~kStride;
    return mask;
}

const char* AntOutput::errorName(Error error) {
    switch (error) {
        case Error::None: return "";
        case Error::CapabilitiesUnavailable: return "capabilities-unavailable";
        case Error::InsufficientChannels: return "insufficient-channels";
        case Error::RadioFailure: return "radio-failure";
    }
    return "unknown";
}

void AntOutput::buildChannels() {
    activeServices_ = services();
    channelCount_ = 0;
    const Channel available[] = {{kFec, 17, 8192}, {kBicyclePower, 11, 8182}, {kHeartRate, 120, 8070},
                                 {kSpeedCadence, 121, 8086}, {kStride, 124, 8134}};
    for (const auto& channel : available) {
        if (activeServices_ & channel.bit) channels_[channelCount_++] = channel;
    }
}

void AntOutput::setClock(Clock clock) { clock_ = std::move(clock); }

std::uint32_t AntOutput::now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void AntOutput::begin() {
    BridgeOutput::begin();
    encoder_.reset();
    sensorEncoder_.reset();
    parser_.reset();
    link_ = Link::WaitingForStick;
    retryPending_ = false;
    pendingCount_ = 0;
    openMask_ = 0;
    error_ = Error::None;
#if defined(ARDUINO) && defined(DEBUG_WEB)
    debugSequence_ = debugMessages_ = debugWriteErrors_ = debugRadioErrors_ = 0;
    debugLastEventMs_ = debugWindowMs_ = now();
#endif
}

void AntOutput::setEnabled(bool enabled) {
    const bool was = this->enabled();
    if (was && !enabled) closeChannel();
    BridgeOutput::setEnabled(enabled);
    if (was != enabled) {
        encoder_.reset();
        sensorEncoder_.reset();
        parser_.reset();
        link_ = Link::WaitingForStick;
        retryPending_ = false;
        displaySeenValid_ = false;
        controlApplied_ = false;
        pendingCount_ = 0;
        openMask_ = 0;
    }
}

AntOutput::Link AntOutput::link() const {
    if (!enabled()) return Link::Disabled;
    if (!transport_) return Link::NoHardware;
    if (!transport_->connected()) return Link::WaitingForStick;
    return link_;
}

const char* AntOutput::linkName(Link link) {
    switch (link) {
        case Link::Disabled: return "disabled";
        case Link::NoHardware: return "no-hardware";
        case Link::WaitingForStick: return "waiting-for-stick";
        case Link::Resetting: return "resetting";
        case Link::Configuring: return "configuring";
        case Link::Open: return "open";
    }
    return "unknown";
}

bool AntOutput::stickConnected() const { return transport_ && transport_->connected(); }

bool AntOutput::displaySeen() const {
    return channelOpen(kFec) && displaySeenValid_ && now() - displaySeenMs_ < kDisplaySeenMs;
}

bool AntOutput::controlActive() const { return displaySeen() && controlApplied_; }

const char* AntOutput::hardware() const { return transport_ ? transport_->description() : ""; }

bool AntOutput::send(const Bytes& bytes) {
    const bool sent = transport_ && !bytes.empty() && transport_->write(bytes.data(), bytes.size());
#if defined(ARDUINO) && defined(DEBUG_WEB)
    if (!sent) ++debugWriteErrors_;
    if (bytes.size() >= 4)
        logMessage("ANT-OUT", bytes[2], bytes.data() + 3, bytes[1], sent ? " tx=accepted" : " tx=failed");
#endif
    return sent;
}

void AntOutput::fail() {
    if (error_ == Error::None) error_ = Error::RadioFailure;
    SF_DEBUG_LOG("[ANT-CP] radio failure error=%s link=%s awaiting=0x%02x retry=%u ms\n",
                 errorName(error_), linkName(link_), awaiting_, static_cast<unsigned>(kRetryMs));
    openMask_ = 0;
    retryPending_ = true;
    retryAtMs_ = now() + kRetryMs;
    link_ = Link::Resetting;
#ifdef ARDUINO
    DBG_SERIAL.printf("[ANT] error: %s\n", errorName(error_));
#endif
}

void AntOutput::closeChannel() {
    if (!stickConnected()) return;
    for (std::uint8_t channel = 0; channel < channelCount_; ++channel) {
        if (openMask_ & channels_[channel].bit) {
            send(encodeMessage(msg::kCloseChannel, Bytes{channel}));
#ifdef ARDUINO
            DBG_SERIAL.printf("[ANT] channel %u closed\n", channels_[channel].bit);
#endif
        }
    }
    if (link_ == Link::Open || link_ == Link::Configuring) send(encodeMessage(msg::kResetSystem, Bytes{0x00}));
}

void AntOutput::startReset() {
    parser_.reset();
    retryPending_ = false;
    awaitingCapabilities_ = false;
    error_ = Error::None;
    channelLimit_ = 0;
    openMask_ = 0;
    displaySeenValid_ = false;
    controlApplied_ = false;
    pendingCount_ = 0;
    encoder_.reset();
    sensorEncoder_.reset();
    buildChannels();
    link_ = Link::Resetting;
    sentMs_ = now();
    if (!send(encodeMessage(msg::kResetSystem, Bytes{0x00}))) fail();
}

void AntOutput::requestCapabilities() {
    awaitingCapabilities_ = true;
    link_ = Link::Configuring;
    sentMs_ = now();
    if (!send(encodeMessage(kRequestMessage, Bytes{0, kCapabilities}))) fail();
}

void AntOutput::sendStep() {
    Bytes payload;
    const std::uint8_t channel = configuringChannel_;
    const auto& spec = channels_[channel];
    switch (step_) {
        case 0:
            awaiting_ = msg::kSetNetworkKey;
            payload = {kNetwork};
            payload.insert(payload.end(), kNetworkKey, kNetworkKey + sizeof(kNetworkKey));
            break;
        case 1:
            awaiting_ = msg::kAssignChannel;
            payload = {channel, fec::kChannelTypeMaster, kNetwork};
            break;
        case 2:
            awaiting_ = msg::kChannelId;
            payload = {channel, static_cast<std::uint8_t>(deviceNumber_ & 0xFFU),
                       static_cast<std::uint8_t>(deviceNumber_ >> 8), spec.deviceType, fec::kTransmissionType};
            break;
        case 3:
            awaiting_ = msg::kChannelPeriod;
            payload = {channel, static_cast<std::uint8_t>(spec.period & 0xFFU),
                       static_cast<std::uint8_t>(spec.period >> 8)};
            break;
        case 4:
            awaiting_ = msg::kChannelRfFrequency;
            payload = {channel, fec::kRfFrequency};
            break;
        default:
            awaiting_ = msg::kOpenChannel;
            payload = {channel};
            break;
    }
    link_ = Link::Configuring;
    sentMs_ = now();
    if (!send(encodeMessage(awaiting_, payload))) fail();
}

void AntOutput::loop() {
    if (!transport_) return;
    transport_->begin();
    if (!enabled()) return;
    if (!transport_->connected()) {
        if (link_ != Link::WaitingForStick) {
            parser_.reset();
            retryPending_ = false;
            displaySeenValid_ = false;
            controlApplied_ = false;
            link_ = Link::WaitingForStick;
            openMask_ = 0;
        }
        return;
    }
    if (activeServices_ != services() && link_ != Link::WaitingForStick) {
        closeChannel();
        link_ = Link::WaitingForStick;
        openMask_ = 0;
        retryPending_ = false;
    }
    std::uint8_t buffer[64];
    for (int reads = 0; reads < 8; ++reads) {
        const std::size_t count = transport_->read(buffer, sizeof(buffer));
        if (count == 0) break;
        Message message;
        for (std::size_t index = 0; index < count; ++index) {
            if (parser_.feed(buffer[index], message)) handleMessage(message);
        }
    }
    const std::uint32_t timestamp = now();
#if defined(ARDUINO) && defined(DEBUG_WEB)
    logStats(timestamp);
#endif
    encoder_.update(trainerState(), profile(), timestamp);
    sensorEncoder_.update(trainerState(), timestamp);
    if (retryPending_) {
        if (reached(timestamp, retryAtMs_)) startReset();
        return;
    }
    switch (link_) {
        case Link::WaitingForStick: startReset(); break;
        case Link::Resetting:
            // Not every stick reports its startup: continue after the timeout.
            if (timestamp - sentMs_ >= kResetTimeoutMs) {
                requestCapabilities();
            }
            break;
        case Link::Configuring:
            if (timestamp - sentMs_ >= kResponseTimeoutMs) {
                SF_DEBUG_LOG("[ANT-CP] timeout awaiting=0x%02x\n", awaitingCapabilities_ ? kCapabilities : awaiting_);
                if (awaitingCapabilities_) error_ = Error::CapabilitiesUnavailable;
                fail();
            }
            break;
        default: break;
    }
}

void AntOutput::handleMessage(const Message& message) {
#if defined(ARDUINO) && defined(DEBUG_WEB)
    logMessage("ANT-IN", message.id, message.payload.data(), message.payload.size());
#endif
    switch (message.id) {
        case msg::kStartup:
            // After our reset, or an unexpected stick reset: (re)configure.
            if (retryPending_) return;
            openMask_ = 0;
            displaySeenValid_ = false;
            controlApplied_ = false;
            pendingCount_ = 0;
            channelLimit_ = 0;
            error_ = Error::None;
            encoder_.reset();
            sensorEncoder_.reset();
            buildChannels();
            requestCapabilities();
            break;
        case kCapabilities:
            if (!awaitingCapabilities_ || link_ != Link::Configuring || message.payload.size() < 4) return;
            awaitingCapabilities_ = false;
            channelLimit_ = message.payload[0];
            if (channelLimit_ < channelCount_) {
                error_ = Error::InsufficientChannels;
                fail();
                return;
            }
            if (channelCount_ == 0) {
                link_ = Link::Open;
                return;
            }
            configuringChannel_ = 0;
            step_ = 0;
            sendStep();
            break;
        case msg::kChannelEvent: handleChannelEvent(message); break;
        case msg::kBroadcastData:
        case msg::kAcknowledgedData:
            if (message.payload.size() < 9 || message.payload[0] >= channelCount_ ||
                channels_[message.payload[0]].bit != kFec || !channelOpen(kFec)) return;
            {
                Page page;
                for (std::size_t index = 0; index < page.size(); ++index) page[index] = message.payload[index + 1];
                handleDisplayPage(page);
            }
            break;
        default: break;
    }
}

void AntOutput::handleChannelEvent(const Message& message) {
    if (message.payload.size() < 3) return;
    const auto channel = message.payload[0];
    const std::uint8_t id = message.payload[1];
    const std::uint8_t result = message.payload[2];
#if defined(ARDUINO) && defined(DEBUG_WEB)
    if ((id == kEventMessageId && (result == code::kTransferTxFailed || result == code::kChannelClosed)) ||
        (id != kEventMessageId && result != code::kResponseNoError))
        ++debugRadioErrors_;
#endif
    if (id != kEventMessageId || result == code::kTransferTxCompleted || result == code::kTransferTxFailed)
        SF_DEBUG_LOG("[ANT-CP] ch=%u responseTo=0x%02x result=0x%02x%s\n", channel, id, result,
                     id == kEventMessageId ? " (radio transfer event, not trainer acceptance)" : "");
    if (id == kEventMessageId) {
        if (channel >= channelCount_ || !channelOpen(channels_[channel].bit)) return;
        if (result == code::kEventTx || result == code::kTransferTxCompleted || result == code::kTransferTxFailed)
            sendNextPage(channel);
        else if (result == code::kChannelClosed)
            fail();
        return;
    }
    if (link_ != Link::Configuring || awaitingCapabilities_ || id != awaiting_ ||
        channel != (step_ == 0 ? kNetwork : configuringChannel_)) return;
    if (result != code::kResponseNoError) {
        fail();
        return;
    }
    if (++step_ <= kSteps) {
        sendStep();
        return;
    }
    openMask_ |= channels_[configuringChannel_].bit;
#ifdef ARDUINO
    DBG_SERIAL.printf("[ANT] channel %u opened\n", channels_[configuringChannel_].bit);
#endif
    sendNextPage(configuringChannel_);
    if (++configuringChannel_ < channelCount_) {
        step_ = 1;
        sendStep();
    } else {
        link_ = Link::Open;
    }
}

void AntOutput::handleDisplayPage(const Page& page) {
    displaySeenValid_ = true;
    displaySeenMs_ = now();
    if (page[0] == fec::page::kRequest) {
        SF_DEBUG_LOG("[ANT-CP] request page=%u repeats=%u acknowledged=%u command=0x%02x\n",
                     page[6], page[5] & 0x7F, page[5] >> 7, page[7]);
        if (page[7] != 0x01) return;  // only "request data page"
        const std::uint8_t repeats = static_cast<std::uint8_t>(page[5] & 0x7FU);
        const PendingRequest request{
            page[6], repeats == 0 ? std::uint8_t{1} : (repeats > kMaxRequestRepeats ? kMaxRequestRepeats : repeats),
            (page[5] & 0x80U) != 0};
        std::size_t slot = 0;
        while (slot < pendingCount_ && pending_[slot].page != request.page) ++slot;
        if (slot == pendingCount_) {
            if (pendingCount_ == kMaxPendingRequests) dropPendingRequest();  // oldest
            slot = pendingCount_++;
        }
        pending_[slot] = request;  // a repeated request refreshes its entry
        return;
    }
    const auto control = fec::decodeControlPage(page);
    if (!control.known) return;
    SF_DEBUG_LOG("[ANT-CP] requested page=%u%s\n", page[0], diagnostics::Summary(kFec, page.data()).c_str());
    auto status = fec::CommandStatus::Pass;
    if (control.hasRequest) {
        status = fec::commandStatusFor(requestControl(control.request));
        controlApplied_ = status == fec::CommandStatus::Pass || controlApplied_;
    }
    encoder_.recordCommand(page[0], status, page);
    SF_DEBUG_LOG("[ANT-CP] page=%u result=%s(0x%02x) command-status recorded\n",
                 page[0], diagnostics::statusName(status), static_cast<unsigned>(status));
}

#if defined(ARDUINO) && defined(DEBUG_WEB)
void AntOutput::logMessage(const char* direction, std::uint8_t id, const std::uint8_t* payload,
                           std::size_t length, const char* result) {
    char prefix[64]{};
    if (direction[4] == 'I') {
        const auto timestamp = now();
        const auto delta = debugSequence_ ? timestamp - debugLastEventMs_ : 0;
        ++debugSequence_;
        ++debugMessages_;
        debugLastEventMs_ = timestamp;
        std::snprintf(prefix, sizeof(prefix), " #%u +%u ms", debugSequence_, delta);
    }
    const bool data = id == msg::kBroadcastData || id == msg::kAcknowledgedData;
    const bool channelMessage = data || id == msg::kChannelEvent || id == msg::kChannelId ||
        id == msg::kAssignChannel || id == msg::kUnassignChannel || id == msg::kChannelPeriod ||
        id == msg::kChannelRfFrequency || id == msg::kOpenChannel || id == msg::kCloseChannel || id == kRequestMessage;
    char channel[80] = "ch=-";
    std::uint8_t service = 0;
    if (channelMessage && length) {
        if (payload[0] < channelCount_) {
            const auto& spec = channels_[payload[0]];
            service = spec.bit;
            std::snprintf(channel, sizeof(channel), "ch=%u device=%u type=%u", payload[0], deviceNumber_, spec.deviceType);
        } else {
            std::snprintf(channel, sizeof(channel), "ch=%u", payload[0]);
        }
    }
    char details[224]{};
    if (data && length >= 9) {
        const diagnostics::Summary summary(service, payload + 1);
        if (service == kSpeedCadence)
            std::snprintf(details, sizeof(details), " page=-%s", summary.c_str());
        else
            std::snprintf(details, sizeof(details), " page=%u%s", service == kHeartRate ? payload[1] & 0x7F : payload[1],
                          summary.c_str());
    } else if (data) {
        std::snprintf(details, sizeof(details), " decode=failed (truncated page)");
    } else if (id == msg::kChannelEvent && length >= 3) {
        std::snprintf(details, sizeof(details), " responseTo=0x%02x result=0x%02x", payload[1], payload[2]);
    }
    // Never expose an overridden private network key in a downloadable log.
    const debuglog::Hex hex(payload, id == msg::kSetNetworkKey ? 0 : length);
    SF_DEBUG_LOG("[%s]%s %s msg=0x%02x len=%zu: %s%s%s\n", direction, prefix, channel, id, length,
                 id == msg::kSetNetworkKey ? "<network key redacted>" : hex.c_str(), details, result);
}

void AntOutput::logStats(std::uint32_t timestamp) {
    const auto window = timestamp - debugWindowMs_;
    if (window < 5000) return;
    char age[40] = "-";
    if (displaySeenValid_) std::snprintf(age, sizeof(age), "%u ms", timestamp - displaySeenMs_);
    SF_DEBUG_LOG("[ANT-IN] stats: messages=%u/%u ms dropped=%u writeErrors=%u radioErrors=%u lastData=%s stale=%u\n",
                 debugMessages_, window, parser_.dropped(), debugWriteErrors_, debugRadioErrors_, age, !displaySeen());
    debugMessages_ = 0;
    debugWindowMs_ = timestamp;
}
#endif

void AntOutput::dropPendingRequest() {
    for (std::size_t index = 1; index < pendingCount_; ++index) pending_[index - 1] = pending_[index];
    if (pendingCount_ > 0) --pendingCount_;
}

void AntOutput::sendNextPage(std::uint8_t channel) {
    encoder_.update(trainerState(), profile(), now());
    sensorEncoder_.update(trainerState(), now());
    Page page{};
    bool requested = false;
    bool acknowledged = false;
    const auto bit = channels_[channel].bit;
    while (bit == kFec && pendingCount_ > 0 && !requested) {
        auto& head = pending_[0];
        requested = encoder_.requested(head.page, deviceNumber_, page);
        if (requested) {
            acknowledged = head.acknowledged;
            --head.repeats;
        }
        if (!requested || head.repeats == 0) dropPendingRequest();
    }
    if (!requested) {
        switch (bit) {
            case kFec: page = encoder_.next(deviceNumber_); break;
            case kBicyclePower: page = sensorEncoder_.bicyclePower(); break;
            case kHeartRate: page = sensorEncoder_.heartRate(); break;
            case kSpeedCadence: page = sensorEncoder_.speedCadence(); break;
            case kStride: page = sensorEncoder_.stride(); break;
            default: return;
        }
    }
    const std::uint8_t response = acknowledged ? msg::kAcknowledgedData : msg::kBroadcastData;
    if (!send(encodeMessage(response, channel, page))) return;
    lastPage_ = page;
    ++pagesSent_;
}
