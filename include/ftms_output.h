#pragma once
#include "bridge_output.h"

// Override publish/loop to encode snapshots as BLE FTMS characteristics.
class FtmsOutput : public BridgeOutput {};
