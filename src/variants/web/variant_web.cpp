// Web variant hooks: Wi-Fi + captive portal + HTTPS/HTTP web server (WebPortal)
// with the REST/WebSocket API (ApiExchange) and ANT+ USB transport.
#ifdef ARDUINO

#include <Arduino.h>

#include "ant_usb_transport.h"
#include "bridge_controller.h"
#include "config_store.h"
#include "variant.h"
#include "web_api.h"
#include "web_portal.h"

namespace {
ApiExchange webApi;
WebPortal* webPortal = nullptr;
AntUsbTransport antUsb;
}  // namespace

namespace variant {

const char* name() { return "web"; }

// ANT+ USB stick on the DevKitC-1 "USB" (USB-OTG) port.
smartfitness::ant::AntTransport* antTransport() { return &antUsb; }

void begin(BridgeController& controller, const BridgeConfig& config) {
    static WebPortal portal(controller, webApi);
    webPortal = &portal;
    webPortal->begin(config.wifiSsid, config.wifiPassword, config.apPassword, config.httpsEnabled);
}

void loop(BridgeController& controller) {
    (void)controller;
    if (webPortal) webPortal->loop();
}

}  // namespace variant

#endif  // ARDUINO
