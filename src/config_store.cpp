#include "config_store.h"

void ConfigStore::begin() {
    // Future: NVS preferences init
}

void ConfigStore::load() {
    // Future: load persisted config
}

void ConfigStore::save() {
    // Future: persist config
}

bool ConfigStore::hasPairedDevice() const {
    return hasPairedDevice_;
}

String ConfigStore::pairedDeviceName() const {
    return pairedDeviceName_;
}
