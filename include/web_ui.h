#pragma once
#include <string>
#include "bridge/bridge_core.hpp"
#include "config_store.h"

class SimulatedDeviceAdapter;
class AntOutput;
class FtmsOutput;

class WebUi {
public:
    WebUi(smartfitness::BridgeCore& bridge, ConfigStore& config, SimulatedDeviceAdapter& adapter,
          AntOutput& ant, FtmsOutput& ftms);
    void begin();
    void loop();
    std::string handleCommand(const std::string& command);
    std::string status() const;

private:
    void applyConfig();
    smartfitness::BridgeCore& bridge_;
    ConfigStore& config_;
    SimulatedDeviceAdapter& adapter_;
    AntOutput& ant_;
    FtmsOutput& ftms_;
    std::string input_;
    bool overflow_ = false;
};
