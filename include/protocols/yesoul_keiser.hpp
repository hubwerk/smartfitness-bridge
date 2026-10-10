#pragma once

// Yesoul bike and Keiser M3i protocols.
//
// Ported from qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift,
// src/devices/yesoulbike/yesoulbike.cpp and src/devices/m3ibike/m3ibike.cpp),
// Copyright (C) Roberto Viola and contributors, licensed under GPL-3.0. This
// port is part of smartfitness-bridge and distributed under the same license.

#include <cstddef>
#include <cstdint>

#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace yesoul {

constexpr std::uint16_t kService = 0xFFF0;
constexpr std::uint16_t kWrite = 0xFFF1;
constexpr std::uint16_t kNotify = 0xFFF4;
constexpr std::uint16_t kManufacturerId = 637;

ble::Bytes initFrame();

struct Frame {
    int resistance = 0;
    float cadence = 0.0F;
    float power = 0.0F;
    float distance = 0.0F;  // km
};
// 12-byte status notification.
bool decode(const std::uint8_t* data, std::size_t length, Frame& out);
inline float speedFromCadence(float cadence) { return 0.37497622F * cadence; }

}  // namespace yesoul

namespace keiser {

struct Broadcast {
    std::uint8_t major = 0;
    std::uint8_t minor = 0;
    std::uint8_t systemId = 0;
    float cadence = 0.0F;    // rpm
    float heartRate = 0.0F;  // bpm
    float power = 0.0F;      // W
    std::uint16_t calories = 0;
    std::uint16_t seconds = 0;
    float distanceKm = 0.0F;
    int gear = 0;  // 0 when the firmware does not report it
};
// Parses M3i manufacturer data. Accepts the payload with or without the
// leading company identifier 0x0102 ("02 01"), like upstream parse_data().
bool parse(const std::uint8_t* data, std::size_t length, Broadcast& out);

}  // namespace keiser

class YesoulSource final : public SourceProtocol {
public:
    const char* id() const override { return "yesoul"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
};

// Advertisement-only: the bike broadcasts its data and is never connected.
class KeiserM3iSource final : public SourceProtocol {
public:
    const char* id() const override { return "keiser-m3i"; }
    bool advertisementOnly() const override { return true; }
    ble::ConnectPlan plan() const override { return {}; }
    bool onConnected(std::uint32_t, std::uint32_t) override { return false; }
    void onNotify(std::size_t, const std::uint8_t*, std::size_t, std::uint32_t) override {}
    void onAdvertisement(const ble::Advertisement& advertisement, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
};

}  // namespace protocols
}  // namespace smartfitness
