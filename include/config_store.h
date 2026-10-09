#pragma once
#include <Arduino.h>
#include "bridge_types.h"

class ConfigStore {
public:
    void begin();
    void load();
    void save();

    bool hasPairedDevice() const;
    String pairedDeviceName() const;

private:
    String pairedDeviceName_;
    bool hasPairedDevice_ = false;
};
