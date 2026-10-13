#pragma once

#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {

bool matchesSportstechF15(const ble::Advertisement&);

class SportstechF15Treadmill final : public SourceProtocol {
public:
    const char* id() const override { return "sportstech-f15"; }
    MachineProfile profile() const override { return MachineProfile::Treadmill; }
    ble::ConnectPlan plan() const override;
    void onWriteEndpoint(const ble::Uuid&, const ble::Uuid&) override;
    bool onConnected(std::uint32_t mask, std::uint32_t nowMs) override;
    void onNotify(std::size_t, const std::uint8_t*, std::size_t, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest&, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    std::optional<TreadmillOperation> treadmillOperation() const override { return operation_; }
    void onWriteFailed(std::uint32_t nowMs) override;
    void reset() override;

private:
    static constexpr std::uint32_t kFreshStatusMs = 5000;
    bool send(const ble::Bytes&);
    bool sendTarget(float speed, float incline);
    bool fresh(std::uint32_t nowMs) const;
    void parse(std::uint32_t nowMs, bool flushShort);
    void accept(const ble::Bytes&, std::uint32_t nowMs);
    void discardWrites();

    ble::Bytes receive_;
    std::optional<TreadmillOperation> operation_;
    std::uint32_t receiveMs_ = 0, pollMs_ = 0, startMs_ = 0;
    float speedTarget_ = 0.8F, inclineTarget_ = 0;
    bool writeFound_ = false, connected_ = false, usable_ = false, delayedStart_ = false;
    bool startupObserved_ = false;
};

}  // namespace protocols
}  // namespace smartfitness
