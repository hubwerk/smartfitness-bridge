#include "config_store.h"

void ConfigStore::begin() {
    load();
}

void ConfigStore::load() {
    config_ = saved_;
}

void ConfigStore::save() {
    saved_ = config_;
}

bool ConfigStore::hasPairedDevice() const {
    return !config_.pairedDeviceName.empty();
}

const std::string& ConfigStore::pairedDeviceName() const {
    return config_.pairedDeviceName;
}

BridgeConfig& ConfigStore::config() { return config_; }
const BridgeConfig& ConfigStore::config() const { return config_; }
void ConfigStore::pairDevice(const std::string& name) { config_.pairedDeviceName = name; }
void ConfigStore::clearPairing() { config_.pairedDeviceName.clear(); }
