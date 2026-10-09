#pragma once
#include <string>
#include "bridge/ftms_roles.hpp"

struct BridgeConfig {
    std::string pairedDeviceName;
    bool antEnabled = true;
    smartfitness::FtmsRoles ftms{false, true};
    bool simulatedTrainer = true;
};

class ConfigStore {
public:
    void begin();
    void load();
    void save();

    bool hasPairedDevice() const;
    const std::string& pairedDeviceName() const;
    BridgeConfig& config();
    const BridgeConfig& config() const;
    void pairDevice(const std::string& name);
    void clearPairing();

private:
    BridgeConfig config_;
    BridgeConfig saved_;
};
