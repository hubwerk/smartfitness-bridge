#pragma once

// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{horizontreadmill,spirittreadmill,solef80treadmill,
// kingsmithr1protreadmill,kingsmithr2treadmill}/ (same-named .cpp/.h);
// src/devices/bluetooth.cpp (advertisement rules).

#include "protocols/source_protocol.hpp"
#include "protocols/registry.hpp"

namespace smartfitness {
namespace protocols {
namespace treadmills {

enum class Model {
    Horizon, HorizonParagon,
    SpiritXT385, SpiritXT485, SpiritXT800,
    SoleF80, SoleF63, SoleF65, SoleTT8,
    WalkingPadR1, WalkingPadRE, WalkingPadR2
};
enum class Cipher { Classic, V2, V3, V4, V5, G1, MXG };
// R2 variants use non-Bluetooth-base UUIDs. Select explicitly when the
// advertisement does not carry a service UUID; never probe by sending controls.
// Service1/Service2 keep standard FED7/8; suffixed variants use 0001/0002FED7/8.
enum class R2Service { Standard, Service1, Service2, Service1Chars2, Service2Chars2, Service1Chars1 };

constexpr int kMatchPriority = 80;
constexpr int kR2MatchPriority = 90;
bool matchesHorizon(const ble::Advertisement&);
bool matchesSpirit(const ble::Advertisement&);
bool matchesSole(const ble::Advertisement&);
bool matchesWalkingPadR1(const ble::Advertisement&);
bool matchesWalkingPadR2(const ble::Advertisement&);
std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

std::uint16_t horizonCrc(const std::uint8_t*, std::size_t);
ble::Bytes horizonFrame(std::uint16_t sequence, std::uint8_t type, std::uint8_t command,
                        const ble::Bytes& payload, bool paragon = false);
ble::Bytes r1Frame(std::uint8_t command, const ble::Bytes& payload);
ble::Bytes r2Encode(const std::string&, Cipher = Cipher::Classic);
bool r2Decode(const ble::Bytes&, std::string&, Cipher = Cipher::Classic);

}  // namespace treadmills

// Register treadmills::descriptorAt() entries: their zero-argument factories
// preserve model/cipher/service in a <=16-character persisted ID. Family-level
// factories below are convenience defaults, not automatic registry entries.
// Non-default cipher entries are manual-only because advertisements do not
// identify the cipher. FTMS advertisements are excluded by every matcher.
class TreadmillSource : public SourceProtocol {
public:
    explicit TreadmillSource(treadmills::Model,
                             treadmills::Cipher = treadmills::Cipher::Classic,
                             treadmills::R2Service = treadmills::R2Service::Standard,
                             float speedLimit = 0);
    const char* id() const override;
    std::uint32_t startupTimeoutMs() const override { return 90000; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t mask, std::uint32_t nowMs) override;
    void onNotify(std::size_t, const std::uint8_t*, std::size_t, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest&, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void onWriteFailed(std::uint32_t nowMs) override;
    void reset() override;
    bool initialized() const { return ready_; }
    float maximumSpeed() const;
    float maximumIncline() const;

private:
    struct Step { ble::Bytes bytes; bool acknowledge; };
    void buildInit();
    void append(const ble::Bytes&, bool acknowledge = true);
    void appendText(const std::string&);
    void acceptFrame(const ble::Bytes&, std::uint32_t);
    void acceptText(const std::string&, std::uint32_t);
    void acknowledged();
    bool send(const ble::Bytes&);
    void driveTarget(std::uint32_t);
    bool horizon() const;
    bool spirit() const;
    bool sole() const;
    bool r1() const;
    bool r2() const;

    treadmills::Model model_;
    treadmills::Cipher cipher_;
    treadmills::R2Service r2Service_;
    float speedLimit_;
    ble::Uuid service_, write_, notify_;
    std::vector<Step> steps_;
    std::size_t step_ = 0, groupStart_ = 0;
    ble::Bytes receive_;
    std::uint32_t receiveMs_ = 0, sentMs_ = 0, pollMs_ = 0;
    std::uint16_t sequence_ = 16;
    unsigned attempts_ = 0, poll_ = 0;
    bool connected_ = false, ready_ = false, waiting_ = false, sent_ = false;
    float speedTarget_ = -1, inclineTarget_ = -1;
    std::uint32_t targetMs_ = 0, targetStartedMs_ = 0;
    int speedDirection_ = 0, inclineDirection_ = 0;
    bool targetWaiting_ = false;
};

class HorizonSource final : public TreadmillSource {
public:
    explicit HorizonSource(bool paragon = false)
        : TreadmillSource(paragon ? treadmills::Model::HorizonParagon : treadmills::Model::Horizon) {}
};
class SpiritSource final : public TreadmillSource {
public:
    explicit SpiritSource(treadmills::Model model = treadmills::Model::SpiritXT385) : TreadmillSource(model) {}
};
class SoleSource final : public TreadmillSource {
public:
    explicit SoleSource(treadmills::Model model = treadmills::Model::SoleF80) : TreadmillSource(model) {}
};
class WalkingPadR1Source final : public TreadmillSource {
public:
    explicit WalkingPadR1Source(bool re = false, float speedLimit = 10)
        : TreadmillSource(re ? treadmills::Model::WalkingPadRE : treadmills::Model::WalkingPadR1,
                          treadmills::Cipher::Classic, treadmills::R2Service::Standard, speedLimit) {}
};
class WalkingPadR2Source final : public TreadmillSource {
public:
    explicit WalkingPadR2Source(treadmills::Cipher cipher = treadmills::Cipher::Classic,
                               treadmills::R2Service service = treadmills::R2Service::Standard,
                               float speedLimit = 12)
        : TreadmillSource(treadmills::Model::WalkingPadR2, cipher, service, speedLimit) {}
};

std::unique_ptr<SourceProtocol> createHorizonTreadmill();
std::unique_ptr<SourceProtocol> createSpiritTreadmill();
std::unique_ptr<SourceProtocol> createSoleTreadmill();
std::unique_ptr<SourceProtocol> createWalkingPadR1();
std::unique_ptr<SourceProtocol> createWalkingPadR2();
std::unique_ptr<SourceProtocol> createTreadmill(const ble::Advertisement&);

}  // namespace protocols
}  // namespace smartfitness
