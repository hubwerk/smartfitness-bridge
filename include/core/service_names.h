#pragma once

#include <string>

#include "ble/ble_types.hpp"

namespace smartfitness {

std::string serviceShortName(const ble::Uuid& uuid);
std::string serviceShortName(const std::string& uuid);
// Bluetooth SIG / well-known vendor characteristic name, or "" when unknown.
std::string characteristicName(const ble::Uuid& uuid);
std::string characteristicName(const std::string& uuid);

}  // namespace smartfitness
