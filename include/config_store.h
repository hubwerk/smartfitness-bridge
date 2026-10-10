#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "bridge/ftms_roles.hpp"
#include "config_backend.h"

// The paired BLE source device (used by the BLE input adapter on boot).
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
    // Schema v2:
    std::string pairedAdapterId;  // protocol id of the paired source (e.g. "ftms", "echelon")
    std::string wifiSsid;         // empty: SoftAP only
    std::string wifiPassword;     // empty (open network) or 8..63 characters
    std::string apPassword = "smartfitness";  // SoftAP WPA2 password, 8..63 printable ASCII characters

    bool operator==(const BridgeConfig& other) const {
        return pairedDeviceName == other.pairedDeviceName && antEnabled == other.antEnabled &&
               ftms.input == other.ftms.input && ftms.output == other.ftms.output &&
               ftmsName == other.ftmsName && simulatedTrainer == other.simulatedTrainer &&
               pairedSource == other.pairedSource && pairedAdapterId == other.pairedAdapterId &&
               wifiSsid == other.wifiSsid && wifiPassword == other.wifiPassword &&
               apPassword == other.apPassword;
    }
    bool operator!=(const BridgeConfig& other) const { return !(*this == other); }
};

enum class ConfigLoadResult { Loaded, Missing, Invalid };
enum class ConfigSaveResult { Written, Unchanged, Invalid, Failed };

class ConfigStore {
public:
    static constexpr std::uint8_t kSchemaVersion = 2;
    static constexpr std::size_t kMaxFtmsNameLength = 20;
    static constexpr std::size_t kMaxPairedNameLength = 32;
    static constexpr std::size_t kMaxAdapterIdLength = 16;
    static constexpr std::size_t kMaxSsidLength = 32;
    static constexpr std::size_t kMinPasswordLength = 8;
    static constexpr std::size_t kMaxPasswordLength = 63;
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
    // Records the paired BLE source (address, adapter, name) and enables the
    // BLE input; persists only that change (other unsaved edits stay unsaved).
    bool pairSource(const PairedSourceDevice& source, const std::string& name, const std::string& adapterId);
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
    static bool isValidAdapterId(const std::string& id);
    static bool isValidSsid(const std::string& ssid);
    // WPA2 passphrase: 8..63 printable ASCII characters; allowEmpty permits an open network.
    static bool isValidPassword(const std::string& password, bool allowEmpty);
    static bool isValidApPassword(const std::string& password);
    static bool isValid(const BridgeConfig& config);
    static std::vector<std::uint8_t> serialize(const BridgeConfig& config);
    // Invalid AP passphrases in otherwise valid records fall back to the default.
    static bool deserialize(const std::vector<std::uint8_t>& data, BridgeConfig& config);

private:
    bool persistPartial(const BridgeConfig& next);
    static std::string sanitizeName(const std::string& name);
    std::unique_ptr<MemoryConfigBackend> ownedBackend_;
    ConfigBackend& backend_;
    BridgeConfig config_;
    BridgeConfig saved_;
    std::vector<std::uint8_t> persisted_;  // record currently in the backend
};
