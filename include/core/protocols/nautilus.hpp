#pragma once

// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{nautilusbike,nautiluselliptical,nautilustreadmill,bowflextreadmill}/
// (same-named .cpp/.h); src/devices/bluetooth.cpp (advertisement rules).
// Revision b1824db5379891d571a7ef2e219d20e6c7ec2c09 of
// src/devices/bluetooth.cpp and src/devices/nautilustreadmill/nautilustreadmill.cpp:
// "SCHWINN T"/"SCHWINN 570T" treadmill names and the 688a15a0 service.
//
// All four upstream classes share the Nautilus GATT layout: one vendor
// service (several alternatives), notify characteristics a46a4a80/6be8f580
// and write characteristic 1717b3c0 receiving a single init frame.
// Upstream sends no resistance/speed/incline commands (stubs only), so every
// adapter here is read-only.

#include <cstddef>
#include <cstdint>

#include "protocols/registry.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace nautilus {

using Bytes = ble::Bytes;

// Frame layouts, selected from the GATT service that was found.
enum class Layout : std::uint8_t {
    BikeMiles,     // nautilusbike, speed in 0.01 mph
    BikeB616,      // nautilusbike B616 service f755c9cf: 0.01 km/h (miles_unit=false)
    EllipticalV0,  // nautiluselliptical bt_variant 0 (14-byte data)
    EllipticalV1,  // bt_variant 1 (12-byte data, Nautilus M7 split frames)
    EllipticalV2,  // bt_variant 2 (14-byte data, mph)
    Treadmill,     // nautilustreadmill (12-byte data)
    Bowflex        // bowflextreadmill (17-byte data)
};

struct Variant {
    Layout layout;
    bool m7;     // name starts "Nautilus M7" (case-insensitive)
    bool t628;   // name starts "NAUTILUS T628": incline not read from frames
};

struct Reading {
    MetricSample speed, cadence, power, heartRate, resistance, incline, totalEnergy;
    bool any() const;
};

// btinit(): 07 01 d3 00 1f 05 01 (identical in all four classes).
Bytes initFrame();
// Decodes one notification; false when the frame is ignored upstream.
bool decode(const Variant& variant, const std::uint8_t* data, std::size_t length, Reading& out);

constexpr int kMatchPriority = 80;
bool matchesBike(const ble::Advertisement&);
bool matchesElliptical(const ble::Advertisement&);
bool matchesM7(const ble::Advertisement&);
bool matchesTreadmill(const ble::Advertisement&);
bool matchesT628(const ble::Advertisement&);

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace nautilus

class NautilusSource final : public SourceProtocol {
public:
    // index into nautilus::descriptorAt()
    explicit NautilusSource(std::size_t model);
    const char* id() const override;
    MachineProfile profile() const override;
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    // Service chosen after connecting (upstream fallback order), -1 when none.
    int selectedService() const { return service_; }
    nautilus::Layout layout() const;

private:
    std::size_t model_;
    int service_ = -1;
};

}  // namespace protocols
}  // namespace smartfitness
