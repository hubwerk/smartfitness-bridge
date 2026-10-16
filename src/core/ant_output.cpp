#include "ant_output.h"

#include <chrono>
#include <utility>

using namespace smartfitness;
using namespace smartfitness::ant;

// ANT+ managed network key (thisisant.com, ANT+ adopter terms apply).
// Override with -DSF_ANT_NETWORK_KEY=0x..,0x.. (8 bytes) for private networks.
#ifndef SF_ANT_NETWORK_KEY
#define SF_ANT_NETWORK_KEY 0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
#endif

namespace {
constexpr std::uint8_t kNetworkKey[8] = {SF_ANT_NETWORK_KEY};
constexpr std::size_t kSteps = 6;
constexpr std::uint8_t kMaxRequestRepeats = 8;
constexpr std::uint8_t kEventMessageId = 0x01;

bool reached(std::uint32_t now, std::uint32_t at) { return static_cast<std::int32_t>(now - at) >= 0; }
}  // namespace

void AntOutput::setTransport(AntTransport* transport) {
    transport_ = transport;
    parser_.reset();
    link_ = Link::WaitingForStick;
}

void AntOutput::setDeviceNumber(std::uint16_t number) { deviceNumber_ = number ? number : 1; }

void AntOutput::setProfileOverride(FtmsOutputProfile profile) { profileOverride_ = profile; }

MachineProfile AntOutput::profile() const { return effectiveOutputProfile(profileOverride_, trainerState().profile); }

void AntOutput::setClock(Clock clock) { clock_ = std::move(clock); }

std::uint32_t AntOutput::now() const {
    if (clock_) return clock_();
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void AntOutput::begin() {
    BridgeOutput::begin();
    encoder_.reset();
    parser_.reset();
    link_ = Link::WaitingForStick;
    retryPending_ = false;
    pendingRepeats_ = 0;
    pendingAcknowledged_ = false;
}

void AntOutput::setEnabled(bool enabled) {
    const bool was = this->enabled();
    if (was && !enabled) closeChannel();
    BridgeOutput::setEnabled(enabled);
    if (was != enabled) {
        encoder_.reset();
        parser_.reset();
        link_ = Link::WaitingForStick;
        retryPending_ = false;
        displaySeenValid_ = false;
        controlApplied_ = false;
        pendingRepeats_ = 0;
        pendingAcknowledged_ = false;
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
    return channelOpen() && displaySeenValid_ && now() - displaySeenMs_ < kDisplaySeenMs;
}

bool AntOutput::controlActive() const { return displaySeen() && controlApplied_; }

const char* AntOutput::hardware() const { return transport_ ? transport_->description() : ""; }

bool AntOutput::send(const Bytes& bytes) {
    return transport_ && !bytes.empty() && transport_->write(bytes.data(), bytes.size());
}

void AntOutput::fail() {
    retryPending_ = true;
    retryAtMs_ = now() + kRetryMs;
    link_ = Link::Resetting;
}

void AntOutput::closeChannel() {
    if (!stickConnected()) return;
    if (link_ == Link::Open) send(encodeMessage(msg::kCloseChannel, Bytes{kChannel}));
    if (link_ == Link::Open || link_ == Link::Configuring) send(encodeMessage(msg::kResetSystem, Bytes{0x00}));
}

void AntOutput::startReset() {
    parser_.reset();
    retryPending_ = false;
    link_ = Link::Resetting;
    sentMs_ = now();
    if (!send(encodeMessage(msg::kResetSystem, Bytes{0x00}))) fail();
}

void AntOutput::sendStep() {
    Bytes payload;
    switch (step_) {
        case 0:
            awaiting_ = msg::kSetNetworkKey;
            payload = {kNetwork};
            payload.insert(payload.end(), kNetworkKey, kNetworkKey + sizeof(kNetworkKey));
            break;
        case 1:
            awaiting_ = msg::kAssignChannel;
            payload = {kChannel, fec::kChannelTypeMaster, kNetwork};
            break;
        case 2:
            awaiting_ = msg::kChannelId;
            payload = {kChannel, static_cast<std::uint8_t>(deviceNumber_ & 0xFFU),
                       static_cast<std::uint8_t>(deviceNumber_ >> 8), fec::kDeviceType, fec::kTransmissionType};
            break;
        case 3:
            awaiting_ = msg::kChannelPeriod;
            payload = {kChannel, static_cast<std::uint8_t>(fec::kChannelPeriod & 0xFFU),
                       static_cast<std::uint8_t>(fec::kChannelPeriod >> 8)};
            break;
        case 4:
            awaiting_ = msg::kChannelRfFrequency;
            payload = {kChannel, fec::kRfFrequency};
            break;
        default:
            awaiting_ = msg::kOpenChannel;
            payload = {kChannel};
            break;
    }
    link_ = Link::Configuring;
    sentMs_ = now();
    if (!send(encodeMessage(awaiting_, payload))) fail();
}

void AntOutput::loop() {
    if (!transport_) return;
    transport_->begin();
    if (!transport_->connected()) {
        if (link_ != Link::WaitingForStick) {
            parser_.reset();
            retryPending_ = false;
            displaySeenValid_ = false;
            controlApplied_ = false;
            link_ = Link::WaitingForStick;
        }
        return;
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
    encoder_.update(trainerState(), profile(), timestamp);
    if (retryPending_) {
        if (reached(timestamp, retryAtMs_)) startReset();
        return;
    }
    switch (link_) {
        case Link::WaitingForStick: startReset(); break;
        case Link::Resetting:
            // Not every stick reports its startup: continue after the timeout.
            if (timestamp - sentMs_ >= kResetTimeoutMs) {
                step_ = 0;
                sendStep();
            }
            break;
        case Link::Configuring:
            if (timestamp - sentMs_ >= kResponseTimeoutMs) fail();
            break;
        default: break;
    }
}

void AntOutput::handleMessage(const Message& message) {
    switch (message.id) {
        case msg::kStartup:
            // After our reset, or an unexpected stick reset: (re)configure.
            if (retryPending_) return;
            step_ = 0;
            sendStep();
            break;
        case msg::kChannelEvent: handleChannelEvent(message); break;
        case msg::kBroadcastData:
        case msg::kAcknowledgedData:
            if (link_ != Link::Open || message.payload.size() < 9 || message.payload[0] != kChannel) return;
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
    if (message.payload.size() < 3 || message.payload[0] != kChannel) return;
    const std::uint8_t id = message.payload[1];
    const std::uint8_t result = message.payload[2];
    if (id == kEventMessageId) {
        if (link_ != Link::Open) return;
        if (result == code::kEventTx || result == code::kTransferTxCompleted || result == code::kTransferTxFailed)
            sendNextPage();
        else if (result == code::kChannelClosed)
            fail();
        return;
    }
    if (link_ != Link::Configuring || id != awaiting_) return;
    if (result != code::kResponseNoError) {
        fail();
        return;
    }
    if (++step_ < kSteps) {
        sendStep();
        return;
    }
    link_ = Link::Open;
    sendNextPage();
}

void AntOutput::handleDisplayPage(const Page& page) {
    displaySeenValid_ = true;
    displaySeenMs_ = now();
    if (page[0] == fec::page::kRequest) {
        if (page[7] != 0x01) return;  // only "request data page"
        const std::uint8_t repeats = static_cast<std::uint8_t>(page[5] & 0x7FU);
        pendingResponse_ = page[6];
        pendingAcknowledged_ = (page[5] & 0x80U) != 0;
        pendingRepeats_ = repeats == 0 ? 1 : (repeats > kMaxRequestRepeats ? kMaxRequestRepeats : repeats);
        return;
    }
    const auto control = fec::decodeControlPage(page);
    if (!control.known) return;
    auto status = fec::CommandStatus::Pass;
    if (control.hasRequest) {
        status = fec::commandStatusFor(requestControl(control.request));
        controlApplied_ = status == fec::CommandStatus::Pass || controlApplied_;
    }
    encoder_.recordCommand(page[0], status, page);
}

void AntOutput::sendNextPage() {
    encoder_.update(trainerState(), profile(), now());
    Page page{};
    bool requested = false;
    while (pendingRepeats_ > 0 && !requested) {
        requested = encoder_.requested(pendingResponse_, deviceNumber_, page);
        if (!requested) pendingRepeats_ = 0;
        else --pendingRepeats_;
    }
    if (!requested) page = encoder_.next(deviceNumber_);
    const std::uint8_t response = requested && pendingAcknowledged_ ? msg::kAcknowledgedData : msg::kBroadcastData;
    if (!send(encodeMessage(response, kChannel, page))) return;
    lastPage_ = page;
    ++pagesSent_;
}
