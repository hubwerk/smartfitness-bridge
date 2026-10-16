#pragma once

// ANT+ Fitness Equipment (FE-C) device profile: data page encoding for the
// fitness equipment (master) side and decoding of the control pages a display
// sends. Every metric the source reports and an FE-C page can carry is
// encoded (see README "ANT+ FE-C output"). Pure C++ (host tested).

#include <cstdint>

#include "ant/ant_message.hpp"
#include "bridge/bridge_core.hpp"

namespace smartfitness {
namespace ant {
namespace fec {

constexpr std::uint8_t kDeviceType = 17;          // ANT+ fitness equipment
constexpr std::uint8_t kTransmissionType = 5;     // independent channel, global pages
constexpr std::uint16_t kChannelPeriod = 8192;    // 4 Hz
constexpr std::uint8_t kRfFrequency = 57;         // 2457 MHz
constexpr std::uint8_t kChannelTypeMaster = 0x10; // bidirectional transmit

namespace page {
constexpr std::uint8_t kGeneralFe = 16;
constexpr std::uint8_t kGeneralSettings = 17;
constexpr std::uint8_t kMetabolic = 18;
constexpr std::uint8_t kTreadmill = 19;
constexpr std::uint8_t kElliptical = 20;
constexpr std::uint8_t kRower = 22;
constexpr std::uint8_t kClimber = 23;
constexpr std::uint8_t kTrainer = 25;
constexpr std::uint8_t kBasicResistance = 48;
constexpr std::uint8_t kTargetPower = 49;
constexpr std::uint8_t kWindResistance = 50;
constexpr std::uint8_t kTrackResistance = 51;
constexpr std::uint8_t kCapabilities = 54;
constexpr std::uint8_t kUserConfiguration = 55;
constexpr std::uint8_t kRequest = 70;
constexpr std::uint8_t kCommandStatus = 71;
constexpr std::uint8_t kManufacturer = 80;
constexpr std::uint8_t kProduct = 81;
}  // namespace page

enum class FeState : std::uint8_t { Off = 1, Ready = 2, InUse = 3, Finished = 4 };

enum class CommandStatus : std::uint8_t { Pass = 0, Fail = 1, NotSupported = 2, Rejected = 3, None = 0xFF };

// FE type in page 16 byte 1 (bits 0-4).
std::uint8_t equipmentType(MachineProfile profile);
// Equipment specific data page (19, 20, 22, 23 or 25).
std::uint8_t specificPage(MachineProfile profile);
CommandStatus commandStatusFor(ControlResult result);

// Control page received from a display. `known` pages are acknowledged with
// a command status; only `hasRequest` pages are routed to the core.
struct ControlPage {
    bool known = false;
    bool hasRequest = false;
    ControlRequest request{Control::Power, 0.0F};
};
ControlPage decodeControlPage(const Page& data);

// Stateful page encoder: keeps the accumulators (event count, accumulated
// power, locally counted elapsed time) and the broadcast schedule.
class Encoder {
public:
    void reset();
    // Updates the source snapshot; `nowMs` advances the local elapsed time
    // used when the source does not report one.
    void update(const TrainerState& state, MachineProfile profile, std::uint32_t nowMs);

    Page generalFeData() const;
    Page generalSettings() const;
    Page metabolic() const;
    Page specific();  // advances the trainer event count / accumulated power
    Page capabilities() const;
    Page commandStatus() const;
    Page manufacturer() const;
    Page product(std::uint32_t serialNumber) const;
    // Any page by number (page 70 requests); false when not supported.
    bool requested(std::uint8_t number, std::uint32_t serialNumber, Page& out);

    bool hasSettings() const;
    bool hasMetabolic() const;
    // Next broadcast page: [16, specific, 16/17/18 rotation, specific] with
    // the common pages 80/81 every 65th message.
    Page next(std::uint32_t serialNumber);

    void recordCommand(std::uint8_t pageNumber, CommandStatus status, const Page& command);
    std::uint8_t lastCommandPage() const { return commandPage_; }
    CommandStatus lastCommandStatus() const { return commandStatus_; }
    FeState state() const;

private:
    std::uint8_t stateBits() const;
    TrainerState state_;
    MachineProfile profile_ = MachineProfile::IndoorBike;
    bool clockStarted_ = false;
    std::uint32_t lastMs_ = 0;
    std::uint32_t localElapsedMs_ = 0;
    std::uint8_t eventCount_ = 0;
    std::uint16_t accumulatedPower_ = 0;
    std::uint32_t counter_ = 0;
    std::uint32_t rotation_ = 0;
    bool commonToggle_ = false;
    std::uint8_t commandPage_ = 0xFF;
    std::uint8_t commandSequence_ = 0xFF;
    CommandStatus commandStatus_ = CommandStatus::None;
    std::array<std::uint8_t, 4> commandData_{0xFF, 0xFF, 0xFF, 0xFF};
};

}  // namespace fec
}  // namespace ant
}  // namespace smartfitness
