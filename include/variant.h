#pragma once

// Build variant selection. Exactly one of the build flags must be set (see the
// PlatformIO environments in platformio.ini):
//   -DSF_VARIANT_WEB  Wi-Fi + web UI + REST/WebSocket API   (env:esp32s3dev)
//   -DSF_VARIANT_HMI  local LVGL HMI on the CrowPanel 1.28" (env:crowpanel128)
// The shared core (include/core, src/core) never includes this header; each
// variant implements the hooks below in src/variants/<variant>/.

#if defined(SF_VARIANT_WEB) && defined(SF_VARIANT_HMI)
#error "Define exactly one of SF_VARIANT_WEB or SF_VARIANT_HMI (both are set)"
#elif !defined(SF_VARIANT_WEB) && !defined(SF_VARIANT_HMI)
#error "Define exactly one of SF_VARIANT_WEB or SF_VARIANT_HMI (none is set)"
#endif

class BridgeController;
struct BridgeConfig;
namespace smartfitness {
namespace ant {
class AntTransport;
}
}  // namespace smartfitness

namespace variant {

// "web" or "hmi".
const char* name();
// ANT radio for the ANT+ FE-C output; nullptr when the variant has none.
// Called from setup() before the core starts.
smartfitness::ant::AntTransport* antTransport();
// Called once from setup() after the core is running and the configuration
// has been applied (controller.begin()).
void begin(BridgeController& controller, const BridgeConfig& config);
// Called from every loop() iteration on the main task, after the core.
void loop(BridgeController& controller);

}  // namespace variant
