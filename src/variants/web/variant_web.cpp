// Web variant hooks: Wi-Fi + captive portal + HTTPS web server (WebPortal)
// with the REST/WebSocket API (ApiExchange) on top of the shared controller.
#ifdef ARDUINO

#include <Arduino.h>

#include "bridge_controller.h"
#include "config_store.h"
#include "variant.h"
#include "web_api.h"
#include "web_portal.h"

namespace {
ApiExchange webApi;
WebPortal* webPortal = nullptr;
}  // namespace

namespace variant {

const char* name() { return "web"; }

void begin(BridgeController& controller, const BridgeConfig& config) {
    static WebPortal portal(controller, webApi);
    webPortal = &portal;
    webPortal->begin(config.wifiSsid, config.wifiPassword, config.apPassword);
    Serial.println("[Web] https://smartfitness.local/ (or the address shown above; self-signed certificate)");
}

void loop(BridgeController& controller) {
    (void)controller;
    if (webPortal) webPortal->loop();
}

}  // namespace variant

#endif  // ARDUINO
