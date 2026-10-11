#pragma once
#include "bridge_output.h"

// Override publish/loop to encode snapshots as ANT+ FE-C pages.
class AntOutput : public BridgeOutput {};
