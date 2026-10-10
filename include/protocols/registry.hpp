#pragma once

// Adapter registry: every input protocol declares how to recognise a device
// from its advertisement and a priority. The scanner picks the highest
// priority match; the user may override the choice when pairing.

#include <cstddef>
#include <memory>
#include <string>

#include "ble/ble_types.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {

struct ProtocolInfo {
    const char* id;
    const char* label;
    int priority;  // higher wins
    bool (*matches)(const ble::Advertisement& advertisement);
    std::unique_ptr<SourceProtocol> (*create)();
    bool controllable;  // can act as trainer (subject to runtime negotiation)
    bool advertisementOnly;
};

std::size_t protocolCount();
const ProtocolInfo& protocolAt(std::size_t index);
const ProtocolInfo* findProtocol(const std::string& id);
// Best (highest priority) match or nullptr when no adapter recognises it.
const ProtocolInfo* detectProtocol(const ble::Advertisement& advertisement);
std::unique_ptr<SourceProtocol> createProtocol(const std::string& id);

}  // namespace protocols
}  // namespace smartfitness
