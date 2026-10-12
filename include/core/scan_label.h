#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ble/ble_types.hpp"

namespace smartfitness {

// Looks up Bluetooth SIG Company Identifier (little-endian first 2 bytes of
// manufacturer data). Returns empty string if unknown.
std::string lookupCompany(std::uint16_t companyId);

// Formats manufacturer label from manufacturer data bytes.
// If manufacturer data has >= 2 bytes:
// - returns known company name (e.g. "Apple") or "Hersteller 0xABCD"
// Returns empty string if manufacturer data has < 2 bytes.
std::string manufacturerLabel(const ble::Bytes& manufacturerData);

// Resolves a service UUID to a human-readable friendly name (e.g. "FTMS",
// "Heart Rate", "Cycling Power", "CSC") or returns uuid.toString() if unknown.
std::string serviceLabel(const ble::Uuid& uuid);

// Checks if a service UUID is a known fitness / sensor service.
bool isKnownService(const ble::Uuid& uuid);

// Finds the most descriptive service label from a list of advertised services.
// Priority: FTMS -> Cycling Power -> CSC -> Heart Rate -> other known -> first service.
std::string bestServiceLabel(const std::vector<ble::Uuid>& services);

// Resolves display label according to priority:
// 1. Advertised name (if not empty)
// 2. Manufacturer from manufacturerData (if >= 2 bytes)
// 3. Service label (if services not empty)
// 4. Address string
std::string scanDeviceLabel(const std::string& name,
                            const ble::Bytes& manufacturerData,
                            const std::vector<ble::Uuid>& services,
                            const ble::BleAddress& address);

// Returns true if the device is classified as relevant:
// has a name OR a known service OR a detected adapter protocol.
bool isRelevantDevice(const std::string& name,
                      const std::string& detectedAdapter,
                      const std::vector<ble::Uuid>& services);

}  // namespace smartfitness
