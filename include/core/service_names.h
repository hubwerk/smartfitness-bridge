#pragma once

#include <string>

#include "ble/ble_types.hpp"

namespace smartfitness {

std::string serviceShortName(const ble::Uuid& uuid);
std::string serviceShortName(const std::string& uuid);

}  // namespace smartfitness
