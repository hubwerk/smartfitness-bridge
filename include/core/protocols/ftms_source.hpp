#pragma once

// FTMS (0x1826) input: Indoor Bike / Treadmill / Cross Trainer / Step Climber /
// Stair Climber / Rower data (every field, mapped onto core metrics),
// Feature and supported-range reads, Control Point with Request Control,
// target power/resistance/inclination/speed and indoor bike simulation.

#include "protocols/source_protocol.hpp"
#include "ftms/ftms_codec.hpp"

#include <array>

namespace smartfitness {
namespace protocols {

class FtmsSource : public SourceProtocol {
public:
    struct Options {
        const char* id = "ftms";
        bool allowControl = true;
        bool bikeDataOnly = false;
    };

    FtmsSource();
    explicit FtmsSource(const Options& options);

    const char* id() const override { return options_.id; }
    MachineProfile profile() const override { return profile_; }
    void onAdvertisement(const ble::Advertisement& advertisement, std::uint32_t nowMs) override;
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onRead(std::size_t index, const std::uint8_t* data, std::size_t length) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    enum class ControlState : std::uint8_t { Unavailable, Requesting, Granted, Denied };
    ControlState controlState() const { return controlState_; }
    const ftms::Features& features() const { return features_; }
    const ftms::SupportedRange& resistanceRange() const { return resistanceRange_; }

    // Characteristic identities (and the indices in the unknown-type fallback
    // plan). Advertised-type plans omit unselected data characteristics.
    enum Subscription : std::uint8_t {
        SubBike, SubTreadmill, SubCrossTrainer, SubRower, SubControlPoint, SubStepClimber, SubStairClimber,
#if defined(ARDUINO) && defined(DEBUG_WEB)
        SubMachineStatus,
#endif
        SubCount
    };
    static constexpr std::uint32_t kDataSubscriptions = (1U << SubBike) | (1U << SubTreadmill) |
        (1U << SubCrossTrainer) | (1U << SubRower) | (1U << SubStepClimber) | (1U << SubStairClimber);
    enum Read : std::uint8_t { ReadFeature, ReadResistanceRange, ReadInclinationRange, ReadPowerRange, ReadCount };

    static constexpr std::uint32_t kControlTimeoutMs = 3000;
    static constexpr std::uint8_t kControlAttempts = 3;

private:
    Subscription subscriptionAt(std::size_t index) const;
    void requestControl(std::uint32_t nowMs);
    void setControlState(ControlState state);
    void handleResponse(const std::uint8_t* data, std::size_t length);
    bool sendControl(const ftms::Bytes& payload);
    void apply(const ftms::MachineData& data, std::uint32_t nowMs);
    float clampTo(const ftms::SupportedRange& range, float value) const;

    Options options_;
    MachineProfile profile_ = MachineProfile::IndoorBike;
    MachineProfile advertisedProfile_ = MachineProfile::IndoorBike;
    bool advertisedProfileValid_ = false;
    std::uint8_t lastMachineTypes_ = 0;
    mutable std::array<Subscription, SubCount> subscriptions_{{
        SubBike, SubTreadmill, SubCrossTrainer, SubRower, SubControlPoint, SubStepClimber, SubStairClimber
#if defined(ARDUINO) && defined(DEBUG_WEB)
        , SubMachineStatus
#endif
    }};
    mutable std::size_t subscriptionCount_ = SubCount;
    std::uint32_t dataSubscribedMask_ = 0;
    bool dataProfileSelected_ = false;
    ftms::Features features_;
    bool featuresRead_ = false;
    ftms::SupportedRange resistanceRange_;
    ftms::SupportedRange inclinationRange_;
    ftms::SupportedRange powerRange_;
    bool controlPointSubscribed_ = false;
    ControlState controlState_ = ControlState::Unavailable;
    std::uint32_t controlRequestedMs_ = 0;
    std::uint8_t controlAttempts_ = 0;
};

}  // namespace protocols
}  // namespace smartfitness
