#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include "ant/ant_message.hpp"
#include "ant/ant_transport.hpp"
#include "ant/fec_codec.hpp"
#include "ant/sensor_codec.hpp"
#include "bridge_output.h"

// Selectable ANT+ master channels on an external radio. FE-C controls are
// routed through the core gate; standalone sensor channels are data-only.
// Pure C++ (host tested with a fake transport).
class AntOutput : public BridgeOutput {
public:
    enum class Link : std::uint8_t { Disabled, NoHardware, WaitingForStick, Resetting, Configuring, Open };
    enum class Error : std::uint8_t { None, CapabilitiesUnavailable, InsufficientChannels, RadioFailure };
    static constexpr std::uint8_t kFec = 1, kBicyclePower = 2, kHeartRate = 4, kSpeedCadence = 8, kStride = 16;

    static constexpr std::uint32_t kResetTimeoutMs = 1000;
    static constexpr std::uint32_t kResponseTimeoutMs = 500;
    static constexpr std::uint32_t kRetryMs = 2000;
    static constexpr std::uint32_t kDisplaySeenMs = 5000;
    static constexpr std::uint8_t kChannel = 0;
    static constexpr std::uint8_t kNetwork = 0;

    // The radio; without one ANT+ output is unavailable.
    void setTransport(smartfitness::ant::AntTransport* transport);
    bool available() const { return transport_ != nullptr; }
    // ANT+ channel device number (0 is replaced by 1).
    void setDeviceNumber(std::uint16_t number);
    std::uint16_t deviceNumber() const { return deviceNumber_; }
    void setProfileOverride(smartfitness::FtmsOutputProfile profile);
    smartfitness::MachineProfile profile() const;
    // Zero selects Auto (FE-C only); stride is effective only for treadmills.
    void setServices(std::uint8_t rawMask);
    std::uint8_t configuredServices() const { return configuredServices_; }
    std::uint8_t services() const;
    Error error() const { return error_; }
    static const char* errorName(Error error);
    std::uint8_t channelLimit() const { return channelLimit_; }

    void begin() override;
    void loop() override;
    void setEnabled(bool enabled) override;

    Link link() const;
    static const char* linkName(Link link);
    bool stickConnected() const;
    bool channelOpen() const { return enabled() && stickConnected() && openMask_ != 0; }
    bool channelOpen(std::uint8_t bit) const { return channelOpen() && (openMask_ & bit) == bit && bit != 0; }
    // A display sent a page within kDisplaySeenMs.
    bool displaySeen() const;
    // A display sent a control page that the core applied.
    bool controlActive() const;
    const char* hardware() const;
    std::uint32_t pagesSent() const { return pagesSent_; }
    const smartfitness::ant::Page& lastPage() const { return lastPage_; }

    using Clock = std::function<std::uint32_t()>;
    void setClock(Clock clock);

private:
    void startReset();
    void sendStep();
    void handleMessage(const smartfitness::ant::Message& message);
    void handleChannelEvent(const smartfitness::ant::Message& message);
    void handleDisplayPage(const smartfitness::ant::Page& page);
    void dropPendingRequest();
    void sendNextPage(std::uint8_t channel);
    void requestCapabilities();
    void buildChannels();
    bool send(const smartfitness::ant::Bytes& bytes);
    void fail();
    void closeChannel();
    std::uint32_t now() const;

#if defined(ARDUINO) && defined(DEBUG_WEB)
    void logMessage(const char* direction, std::uint8_t id, const std::uint8_t* payload,
                    std::size_t length, const char* result = "");
    void logStats(std::uint32_t timestamp);
    std::uint32_t debugSequence_ = 0;
    std::uint32_t debugLastEventMs_ = 0;
    std::uint32_t debugWindowMs_ = 0;
    std::uint32_t debugMessages_ = 0;
    std::uint32_t debugWriteErrors_ = 0;
    std::uint32_t debugRadioErrors_ = 0;
#endif

    smartfitness::ant::AntTransport* transport_ = nullptr;
    smartfitness::ant::MessageParser parser_;
    smartfitness::ant::fec::Encoder encoder_;
    smartfitness::ant::sensor::Encoder sensorEncoder_;
    struct Channel {
        std::uint8_t bit = 0;
        std::uint8_t deviceType = 0;
        std::uint16_t period = 0;
    };
    std::array<Channel, 5> channels_{};
    std::uint8_t channelCount_ = 0;
    std::uint8_t configuringChannel_ = 0;
    std::uint8_t openMask_ = 0;
    std::uint8_t configuredServices_ = 0;
    std::uint8_t activeServices_ = 0;
    std::uint8_t channelLimit_ = 0;
    bool awaitingCapabilities_ = false;
    Error error_ = Error::None;
    Link link_ = Link::WaitingForStick;
    std::size_t step_ = 0;
    std::uint8_t awaiting_ = 0;
    std::uint32_t sentMs_ = 0;
    std::uint32_t retryAtMs_ = 0;
    bool retryPending_ = false;
    std::uint16_t deviceNumber_ = 1;
    smartfitness::FtmsOutputProfile profileOverride_ = smartfitness::FtmsOutputProfile::Auto;
    bool displaySeenValid_ = false;
    std::uint32_t displaySeenMs_ = 0;
    bool controlApplied_ = false;
    // Pages requested via page 70, answered in arrival order.
    struct PendingRequest {
        std::uint8_t page = 0;
        std::uint8_t repeats = 0;
        bool acknowledged = false;
    };
    static constexpr std::size_t kMaxPendingRequests = 4;
    std::array<PendingRequest, kMaxPendingRequests> pending_{};
    std::size_t pendingCount_ = 0;
    std::uint32_t pagesSent_ = 0;
    smartfitness::ant::Page lastPage_{};
    Clock clock_;
};
