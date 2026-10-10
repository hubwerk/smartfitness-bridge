#pragma once

// Pure C++ BLE value types shared by the central (input) role, the protocol
// adapters and the host tests. No Arduino or NimBLE dependency.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smartfitness {
namespace ble {

using Bytes = std::vector<std::uint8_t>;

// 128-bit UUID stored in canonical (big-endian, as printed) byte order.
struct Uuid {
    std::array<std::uint8_t, 16> bytes{};

    static Uuid from16(std::uint16_t shortUuid);
    // Accepts "180d", "0x180D" or the 36-character canonical form (any case).
    static bool parse(const std::string& text, Uuid& out);
    // Helper for compile-time-known UUID strings; returns a zero UUID when invalid.
    static Uuid fromString(const char* text);

    // True for UUIDs derived from the Bluetooth base UUID.
    bool isShort() const;
    std::uint16_t shortValue() const;
    // "0x180d" for short UUIDs, the canonical lowercase form otherwise.
    std::string toString() const;
    std::string toCanonicalString() const;
    bool isZero() const;

    bool operator==(const Uuid& other) const { return bytes == other.bytes; }
    bool operator!=(const Uuid& other) const { return !(*this == other); }
};

// Address in display order (most significant byte first, "aa:bb:cc:dd:ee:ff").
struct BleAddress {
    std::array<std::uint8_t, 6> bytes{};
    std::uint8_t type = 0;  // 0 public, 1 random (NimBLE address types 0..3)

    static bool parse(const std::string& text, BleAddress& out, std::uint8_t type = 0);
    std::string toString() const;
    bool isZero() const;
    // Address equality ignores the type, which some stacks report inconsistently.
    bool operator==(const BleAddress& other) const { return bytes == other.bytes; }
    bool operator!=(const BleAddress& other) const { return !(*this == other); }
};

constexpr std::size_t kMaxAdvertisedName = 31;
constexpr std::size_t kMaxAdvertisedServices = 6;
constexpr std::size_t kMaxManufacturerData = 31;

// One advertisement/scan response as reported by the scanner. Fixed size so it
// can be queued from the BLE task without heap allocation.
struct ScanReport {
    BleAddress address;
    std::int8_t rssi = 0;
    bool connectable = true;
    std::uint8_t nameLength = 0;
    std::array<char, kMaxAdvertisedName> name{};
    std::uint8_t serviceCount = 0;
    std::array<Uuid, kMaxAdvertisedServices> services{};
    std::uint8_t manufacturerLength = 0;
    std::array<std::uint8_t, kMaxManufacturerData> manufacturer{};
    std::uint8_t ftmsServiceDataLength = 0;
    std::array<std::uint8_t, 3> ftmsServiceData{};

    void setName(const std::string& value);
    std::string nameString() const;
    bool addService(const Uuid& uuid);
    void setManufacturerData(const std::uint8_t* data, std::size_t length);
    void setFtmsServiceData(const std::uint8_t* data, std::size_t length);
};

// Heap-friendly advertisement view used by the protocol matchers.
struct Advertisement {
    BleAddress address;
    std::string name;
    int rssi = 0;
    std::vector<Uuid> services;
    Bytes manufacturerData;  // includes the 2-byte company identifier (little endian)
    Bytes ftmsServiceData;   // FTMS flags and little-endian Fitness Machine Type

    static Advertisement from(const ScanReport& report);
    bool hasService(const Uuid& uuid) const;
    bool hasService(std::uint16_t shortUuid) const { return hasService(Uuid::from16(shortUuid)); }
};

constexpr std::size_t kMaxWriteLength = 32;

struct GattWrite {
    Uuid service;
    Uuid characteristic;
    std::uint8_t length = 0;
    std::array<std::uint8_t, kMaxWriteLength> data{};
    bool withResponse = true;

    static GattWrite make(const Uuid& service, const Uuid& characteristic, const Bytes& payload,
                          bool withResponse = true);
    Bytes payload() const { return Bytes(data.begin(), data.begin() + length); }
};

struct Subscription {
    Uuid service;
    Uuid characteristic;
    bool indicate = false;  // false: notifications
};

struct ReadRequest {
    Uuid service;
    Uuid characteristic;
};

constexpr std::size_t kMaxSubscriptions = 8;
constexpr std::size_t kMaxReads = 6;

// What the central must do after connecting: subscribe and read. Results are
// reported by index (bit N of the connected mask = subscription N succeeded).
struct ConnectPlan {
    // Opt-in dynamic subscription discovery for protocols whose upstream
    // enumerates GATT. Writes are restricted to these characteristic UUIDs.
    bool discoverNotifications = false;
    std::array<Uuid, 2> discoveryWriteCharacteristics{};
    std::uint8_t subscriptionCount = 0;
    std::array<Subscription, kMaxSubscriptions> subscriptions{};
    std::uint8_t readCount = 0;
    std::array<ReadRequest, kMaxReads> reads{};

    bool subscribe(const Uuid& service, const Uuid& characteristic, bool indicate = false);
    bool read(const Uuid& service, const Uuid& characteristic);
};

}  // namespace ble
}  // namespace smartfitness
