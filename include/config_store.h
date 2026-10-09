#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "bridge/ftms_roles.hpp"
#include "config_backend.h"

// Reserved for the future BLE input adapter: the paired source device.
struct PairedSourceDevice {
    bool valid = false;
    std::uint8_t addressType = 0;  // NimBLE/BLE address type (0..3)
    std::array<std::uint8_t, 6> address{};

    bool operator==(const PairedSourceDevice& other) const {
        return valid == other.valid && addressType == other.addressType && address == other.address;
    }
    bool operator!=(const PairedSourceDevice& other) const { return !(*this == other); }
};

struct BridgeConfig {
    std::string pairedDeviceName;
    bool antEnabled = true;
    smartfitness::FtmsRoles ftms{false, true};
    std::string ftmsName;  // advertised FTMS output name; empty uses the default
    bool simulatedTrainer = true;
    PairedSourceDevice pairedSource;

    bool operator==(const BridgeConfig& other) const {
        return pairedDeviceName == other.pairedDeviceName && antEnabled == other.antEnabled &&
               ftms.input == other.ftms.input && ftms.output == other.ftms.output &&
               ftmsName == other.ftmsName && simulatedTrainer == other.simulatedTrainer &&
               pairedSource == other.pairedSource;
    }
    bool operator!=(const BridgeConfig& other) const { return !(*this == other); }
};

enum class ConfigLoadResult { Loaded, Missing, Invalid };
enum class ConfigSaveResult { Written, Unchanged, Invalid, Failed };

class ConfigStore {
public:
    static constexpr std::uint8_t kSchemaVersion = 1;
    static constexpr std::size_t kMaxFtmsNameLength = 20;
    static constexpr std::size_t kMaxPairedNameLength = 32;
    static constexpr std::uint8_t kMaxAddressType = 3;

    // Without a backend, configuration is kept in a private RAM backend.
    ConfigStore();
    explicit ConfigStore(ConfigBackend& backend);
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    ConfigLoadResult begin();
    // Reloads from the backend; missing or invalid records fall back to defaults.
    ConfigLoadResult load();
    // Writes only when the stored record would change.
    ConfigSaveResult save();
    // Clears the paired device and persists only that change.
    bool unpair();
    // Erases all stored configuration and restores defaults.
    bool factoryReset();
    bool hasUnsavedChanges() const;

    bool hasPairedDevice() const;
    const std::string& pairedDeviceName() const;
    BridgeConfig& config();
    const BridgeConfig& config() const;
    // Records the paired device name (sanitized to printable ASCII, truncated); not persisted until save().
    void pairDevice(const std::string& name);

    static bool isValidFtmsName(const std::string& name);
    static bool isValid(const BridgeConfig& config);
    static std::vector<std::uint8_t> serialize(const BridgeConfig& config);
    static bool deserialize(const std::vector<std::uint8_t>& data, BridgeConfig& config);

private:
    std::unique_ptr<MemoryConfigBackend> ownedBackend_;
    ConfigBackend& backend_;
    BridgeConfig config_;
    BridgeConfig saved_;
    std::vector<std::uint8_t> persisted_;  // record currently in the backend
};
