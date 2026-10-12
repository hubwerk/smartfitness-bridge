#include "scan_label.h"

#include <cstdio>

namespace smartfitness {
namespace {

struct CompanyEntry {
    std::uint16_t id;
    const char* name;
};

// Bluetooth SIG Company Identifiers for common device and fitness manufacturers.
constexpr CompanyEntry kCompanies[] = {
    {0x0001, "Nokia"},
    {0x0006, "Microsoft"},
    {0x000A, "Qualcomm"},
    {0x000D, "Texas Instruments"},
    {0x0047, "Sony"},
    {0x004C, "Apple"},
    {0x0056, "Huawei"},
    {0x0059, "Nordic"},
    {0x0075, "Samsung"},
    {0x0087, "Garmin"},
    {0x0089, "Tacx"},
    {0x009E, "Bose"},
    {0x00D2, "Dialog Semiconductor"},
    {0x00E0, "Google"},
    {0x0106, "Polar"},
    {0x0157, "Huami/Xiaomi"},
    {0x0182, "Keiser"},
    {0x01A7, "Suunto"},
    {0x01C7, "Wahoo"},
    {0x0211, "Wahoo Fitness"},
    {0x0269, "Decathlon"},
    {0x027D, "Yesoul"},
    {0x02CD, "Elite"},
    {0x02E5, "Espressif"},
    {0x038F, "Xiaomi"},
    {0x0499, "Ruuvi"},
};

const ble::Uuid kTacxFecService = ble::Uuid::fromString("6e40fec1-b5a3-f393-e0a9-e50e24dcca9e");
const ble::Uuid kEliteService = ble::Uuid::fromString("347b0001-7635-408b-8918-8ff3949ce592");
const ble::Uuid kConcept2Service = ble::Uuid::fromString("ce060030-43e5-11e4-916c-0800200c9a66");
const ble::Uuid kIfitService = ble::Uuid::fromString("1007e000-0000-0000-0000-00000000000a");
const ble::Uuid kBowflex1Service = ble::Uuid::fromString("edff9e80-cad7-11e5-ab63-0002a5d5c51b");
const ble::Uuid kBowflex2Service = ble::Uuid::fromString("b1216c2e-464e-4c46-b2b4-0b5c8eb23dae");
const ble::Uuid kWalkingPad1 = ble::Uuid::fromString("00011234-0000-1000-8000-00805f9b34fb");
const ble::Uuid kWalkingPad2 = ble::Uuid::fromString("00021234-0000-1000-8000-00805f9b34fb");

}  // namespace

std::string lookupCompany(std::uint16_t companyId) {
    for (const auto& entry : kCompanies) {
        if (entry.id == companyId) return entry.name;
    }
    return "";
}

std::string manufacturerLabel(const ble::Bytes& manufacturerData) {
    if (manufacturerData.size() < 2) return "";
    const std::uint16_t companyId = static_cast<std::uint16_t>(manufacturerData[0]) |
                                    (static_cast<std::uint16_t>(manufacturerData[1]) << 8);
    const std::string name = lookupCompany(companyId);
    if (!name.empty()) return name;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "Hersteller 0x%04X", companyId);
    return buffer;
}

bool isKnownService(const ble::Uuid& uuid) {
    if (uuid.isShort()) {
        const std::uint16_t s = uuid.shortValue();
        if (s == 0x1826 || s == 0x1818 || s == 0x1816 || s == 0x180D || s == 0x1814) {
            return true;
        }
    }
    return uuid == kTacxFecService || uuid == kEliteService || uuid == kConcept2Service ||
           uuid == kIfitService || uuid == kBowflex1Service || uuid == kBowflex2Service ||
           uuid == kWalkingPad1 || uuid == kWalkingPad2;
}

std::string serviceLabel(const ble::Uuid& uuid) {
    if (uuid.isShort()) {
        const std::uint16_t s = uuid.shortValue();
        if (s == 0x1826) return "FTMS";
        if (s == 0x1818) return "Cycling Power";
        if (s == 0x1816) return "CSC";
        if (s == 0x180D) return "Heart Rate";
        if (s == 0x1814) return "Running Speed/Cadence";
        if (s == 0xFE00) return "WalkingPad";
    }
    if (uuid == kTacxFecService) return "Tacx FE-C";
    if (uuid == kEliteService) return "Elite Trainer";
    if (uuid == kConcept2Service) return "Concept2 PM5";
    if (uuid == kIfitService) return "iFit";
    if (uuid == kBowflex1Service || uuid == kBowflex2Service) return "Bowflex";
    if (uuid == kWalkingPad1 || uuid == kWalkingPad2) return "WalkingPad";
    return "";
}

std::string bestServiceLabel(const std::vector<ble::Uuid>& services) {
    if (services.empty()) return "";
    // High priority standard fitness services
    for (const auto& s : services) {
        if (s.isShort() && s.shortValue() == 0x1826) return "FTMS";
    }
    for (const auto& s : services) {
        if (s.isShort() && s.shortValue() == 0x1818) return "Cycling Power";
    }
    for (const auto& s : services) {
        if (s.isShort() && s.shortValue() == 0x1816) return "CSC";
    }
    for (const auto& s : services) {
        if (s.isShort() && s.shortValue() == 0x180D) return "Heart Rate";
    }
    // Any other known service
    for (const auto& s : services) {
        if (isKnownService(s)) {
            const std::string lbl = serviceLabel(s);
            if (!lbl.empty()) return lbl;
        }
    }
    // Fallback to first service UUID string
    return services.front().toString();
}

std::string scanDeviceLabel(const std::string& name,
                            const ble::Bytes& manufacturerData,
                            const std::vector<ble::Uuid>& services,
                            const ble::BleAddress& address) {
    if (!name.empty()) return name;
    if (manufacturerData.size() >= 2) {
        const std::string m = manufacturerLabel(manufacturerData);
        if (!m.empty()) return m;
    }
    if (!services.empty()) {
        const std::string s = bestServiceLabel(services);
        if (!s.empty()) return s;
    }
    return address.toString();
}

bool isRelevantDevice(const std::string& name,
                      const std::string& detectedAdapter,
                      const std::vector<ble::Uuid>& services) {
    if (!name.empty()) return true;
    if (!detectedAdapter.empty()) return true;
    for (const auto& service : services) {
        if (isKnownService(service)) return true;
    }
    return false;
}

}  // namespace smartfitness
