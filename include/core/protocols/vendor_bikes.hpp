#pragma once

// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{flywheelbike,solebike,kettlerc12bike,technogymbike,inspirebike,
// keepbike,mcfbike,xcxbike,chronobike,volavabike,ultrasportbike,skandikawiribike,
// pitpatbike,freebeatboombike,mepanelbike,apexbike,npecablebike,
// cycleopsphantombike,renphobike,lifespanbike,horizongr7bike,cscbike}/
// (same-named .cpp/.h); src/devices/bluetooth.cpp (advertisement rules).
//
// One table-driven source covers the proprietary vendor bikes: every model is
// a row (UUIDs, init/poll frames, decoder, control range) so the shared engine
// stays small in flash. Name-routed FTMS bikes (Renpho, Horizon GR7) reuse
// FtmsSource with their own persisted IDs.

#include <cstddef>
#include <cstdint>
#include <string>

#include "protocols/registry.hpp"
#include "protocols/sensor_codec.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace vendor_bikes {

// Append only: values index the model table.
enum class Model : std::uint8_t {
    Flywheel, LifeFitnessIc5, SoleBike, SoleR92, KettlerC12, Technogym, Inspire, KeepBike, Mcf, Xcx,
    Chrono, Volava, Ultrasport, SkandikaWiri, SkandikaX2000, PitPat, FreebeatBoom, Mepanel, Apex,
    NpeCable, CycleOps, CscBike, LifespanBike, Count
};

constexpr int kMatchPriority = 80;

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);
const char* modelId(Model model);

// Frame builders and formulas shared by the sources and the host tests.
ble::Bytes skandikaPoll(std::uint8_t level);
ble::Bytes pitpatResistance(std::uint8_t level);
ble::Bytes freebeatResistance(std::uint8_t level);
ble::Bytes technogymResistance(int level);
ble::Bytes technogymPower(int watts);
ble::Bytes cycleopsPower(int watts);
float apexPower(int resistance, float cadence);
float inspirePower(int resistance, float cadence);
int bcd(std::uint8_t value);

// Flywheel / Life Fitness IC stream framing: FF len id data.. crc 55.
class FlywheelDecoder {
public:
    // Feeds bytes; returns true when a complete frame ended in this chunk.
    bool feed(const std::uint8_t* data, std::size_t length);
    std::uint8_t id() const { return id_; }
    std::size_t size() const { return size_; }
    const std::uint8_t* data() const { return buffer_; }
    void reset();

private:
    enum State : std::uint8_t { Sync, Length, Id, Data, Checksum, End };
    State state_ = Sync;
    int remaining_ = 0;
    std::uint8_t id_ = 0;
    std::size_t size_ = 0;
    std::uint8_t buffer_[64] = {};
};

}  // namespace vendor_bikes

class VendorBikeSource final : public SourceProtocol {
public:
    explicit VendorBikeSource(vendor_bikes::Model model);
    const char* id() const override;
    ble::ConnectPlan plan() const override;
    void onAdvertisement(const ble::Advertisement& advertisement, std::uint32_t nowMs) override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void onWriteFailed(std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    bool ready() const { return ready_; }

    // Kinds of subscriptions in plan(); index -> kind is fixed per model.
    enum Kind : std::uint8_t {
        Vendor, FtmsBike, Power, Csc, HeartRate, Nus, KResistance, KPower, KSpeed, KResistance2, KDistance
    };

private:
    std::size_t buildPlan(ble::ConnectPlan& plan, Kind* kinds) const;
    bool send(const std::uint8_t* data, std::size_t length);
    bool send(const ble::Bytes& bytes) { return send(bytes.data(), bytes.size()); }
    void sendPoll(std::uint32_t nowMs);
    void decodeVendor(const std::uint8_t* d, std::size_t n, std::uint32_t nowMs);
    void decodeStandard(Kind kind, const std::uint8_t* d, std::size_t n, std::uint32_t nowMs);
    void setPowerCadence(float power, float cadence);
    void useRate(sensors::RevolutionRate& rate, float divisor);

    vendor_bikes::Model model_;
    Kind kinds_[ble::kMaxSubscriptions] = {};
    std::size_t kindCount_ = 0;
    vendor_bikes::FlywheelDecoder stream_;
    sensors::RevolutionRate crank_{1024, 16};
    sensors::RevolutionRate wheel_{2048, 32};
    sensors::RevolutionRate cscCrank_{1024, 16};
    sensors::RevolutionRate cscWheel_{1024, 32};
    sensors::RevolutionRate* cadenceRate_ = nullptr;
    float cadenceDivisor_ = 1.0F;
    std::string name_;
    std::size_t step_ = 0, pollIndex_ = 0;
    std::uint32_t nextStepMs_ = 0, lastPollMs_ = 0, readyAtMs_ = 0, lastCpsPowerMs_ = 0;
    std::uint32_t pollTicks_ = 0;
    std::uint8_t lastCommand_ = 0, zeroFilter_ = 0, target_ = 1;
    int resistance_ = 0;
    float cadence_ = 0;
    bool connected_ = false, ready_ = false, legacy_ = false, alternateWrite_ = false;
    bool cpsPowerSeen_ = false;
};

std::unique_ptr<SourceProtocol> createVendorBike(vendor_bikes::Model model);

}  // namespace protocols
}  // namespace smartfitness
