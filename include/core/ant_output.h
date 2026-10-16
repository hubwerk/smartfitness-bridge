#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include "ant/ant_message.hpp"
#include "ant/ant_transport.hpp"
#include "ant/fec_codec.hpp"
#include "bridge_output.h"

// ANT+ FE-C output: opens one bidirectional ANT+ master channel (device type
// 17) on an external ANT radio (AntTransport) and broadcasts FE-C data pages
// at 4 Hz. Control pages from the display (basic resistance, target power,
// track resistance) are routed through the core gate like FTMS controls.
// Pure C++ (host tested with a fake transport).
class AntOutput : public BridgeOutput {
public:
    enum class Link : std::uint8_t { Disabled, NoHardware, WaitingForStick, Resetting, Configuring, Open };

    static constexpr std::uint32_t kResetTimeoutMs = 1000;
    static constexpr std::uint32_t kResponseTimeoutMs = 500;
    static constexpr std::uint32_t kRetryMs = 2000;
    static constexpr std::uint32_t kDisplaySeenMs = 5000;
    static constexpr std::uint8_t kChannel = 0;
    static constexpr std::uint8_t kNetwork = 0;

    // The radio; without one ANT+ FE-C is unavailable (HMI variant).
    void setTransport(smartfitness::ant::AntTransport* transport);
    bool available() const { return transport_ != nullptr; }
    // ANT+ channel device number (0 is replaced by 1).
    void setDeviceNumber(std::uint16_t number);
    std::uint16_t deviceNumber() const { return deviceNumber_; }
    void setProfileOverride(smartfitness::FtmsOutputProfile profile);
    smartfitness::MachineProfile profile() const;

    void begin() override;
    void loop() override;
    void setEnabled(bool enabled) override;

    Link link() const;
    static const char* linkName(Link link);
    bool stickConnected() const;
    bool channelOpen() const { return enabled() && link_ == Link::Open; }
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
    void sendNextPage();
    bool send(const smartfitness::ant::Bytes& bytes);
    void fail();
    void closeChannel();
    std::uint32_t now() const;

    smartfitness::ant::AntTransport* transport_ = nullptr;
    smartfitness::ant::MessageParser parser_;
    smartfitness::ant::fec::Encoder encoder_;
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
    std::uint8_t pendingResponse_ = 0;  // page requested via page 70
    std::uint8_t pendingRepeats_ = 0;
    bool pendingAcknowledged_ = false;
    std::uint32_t pagesSent_ = 0;
    smartfitness::ant::Page lastPage_{};
    Clock clock_;
};
