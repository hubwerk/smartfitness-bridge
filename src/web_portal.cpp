#ifdef ARDUINO

#include "web_portal.h"

#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "generated/web_assets.h"
#include "config_store.h"
#include "web_request.h"

namespace {
constexpr std::uint16_t kDnsPort = 53;
constexpr std::uint32_t kPolicyIntervalMs = 1000;
constexpr const char* kBodyAllocationFailed = "sf-body-allocation-failed";

void sendJson(AsyncWebServerRequest* request, int status, const std::string& body) {
    AsyncWebServerResponse* response = request->beginResponse(status, "application/json", body.c_str());
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

// The library frees _tempObject, so metadata and bytes share one calloc allocation.
void collectBody(AsyncWebServerRequest* request, std::uint8_t* data, std::size_t length, std::size_t index,
                 std::size_t total) {
    if (total > ApiExchange::kMaxBodySize || request->hasHeader("Transfer-Encoding") ||
        request->getAttribute(kBodyAllocationFailed, false)) return;
    if (!request->_tempObject) {
        if (index != 0) return;
        void* memory = std::calloc(sizeof(web::BodyReceipt) + total + 1, 1);
        if (!memory) {
            request->setAttribute(kBodyAllocationFailed, true);
            return;
        }
        request->_tempObject = new (memory) web::BodyReceipt{total, 0, false};
    }
    auto* receipt = static_cast<web::BodyReceipt*>(request->_tempObject);
    receipt->append(reinterpret_cast<char*>(receipt + 1), data, length, index, total);
}

std::string bodyOf(AsyncWebServerRequest* request) {
    if (!request->_tempObject) return std::string();
    const auto* receipt = static_cast<const web::BodyReceipt*>(request->_tempObject);
    return std::string(reinterpret_cast<const char*>(receipt + 1), receipt->received);
}
}  // namespace

struct WebPortal::Impl {
    AsyncWebServer server{80};
    AsyncWebSocket socket{"/ws"};
    DNSServer dns;
    bool dnsRunning = false;
    bool mdnsRunning = false;
    bool scanPending = false;
    bool configPending = false;

    bool broadcast(const std::string& message) {
        if (!web::canBroadcast(socket.count(), socket.availableForWriteAll())) return false;
        socket.textAll(message.c_str());
        return true;
    }
};

WebPortal::WebPortal(WebUi& ui, ApiExchange& api) : ui_(ui), api_(api) {}

void WebPortal::begin(const std::string& ssid, const std::string& password, const std::string& apPassword) {
    if (started_) return;
    started_ = true;
    impl_ = new Impl;
    ssid_ = ssid;
    password_ = password;
    apPassword_ = apPassword;

    std::array<std::uint8_t, 6> mac{};
    WiFi.macAddress(mac.data());
    apSsid_ = softApSsid(mac);
    WiFi.persistent(false);  // credentials live in the bridge configuration (NVS), not the Wi-Fi driver
    WiFi.setHostname(kHostname);
    // Modem sleep must stay enabled while Bluetooth is active (coexistence).
    WiFi.setSleep(true);
    applyMode(policy_.begin(!ssid_.empty(), millis()));

    AsyncWebServer& server = impl_->server;
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response =
            request->beginResponse(200, "text/html", kIndexHtmlGz, sizeof(kIndexHtmlGz));
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
    server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, api_.snapshot(ApiExchange::Snapshot::Status));
    });
    server.on("/api/scan/results", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, api_.snapshot(ApiExchange::Snapshot::Scan));
    });
    server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
        sendJson(request, 200, api_.snapshot(ApiExchange::Snapshot::Config));
    });
    server.on("/api/result", HTTP_GET, [this](AsyncWebServerRequest* request) {
        const AsyncWebParameter* id = request->getParam("id");
        const ApiResponse response = api_.result(id ? static_cast<std::uint32_t>(std::strtoul(id->value().c_str(), nullptr, 10)) : 0);
        sendJson(request, response.status, response.body);
    });

    static const char* const kPostEndpoints[] = {"scan",   "pair", "unpair",  "connect",       "disconnect",
                                                 "simulate", "control", "config", "factory-reset", "restart"};
    for (const char* endpoint : kPostEndpoints) {
        const std::string name = endpoint;
        server.on(
            AsyncURIMatcher::exact((std::string("/api/") + name).c_str()), HTTP_POST,
            [this, name](AsyncWebServerRequest* request) {
                const bool hasOrigin = request->hasHeader("Origin");
                const std::string origin = hasOrigin ? request->getHeader("Origin")->value().c_str() : "";
                const int policy = web::postRequestStatus(request->contentType().c_str(), hasOrigin, origin,
                    WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str(),
                    request->hasHeader("Transfer-Encoding"));
                if (policy != 200) {
                    sendJson(request, policy, policy == 415 ? "{\"error\":\"Content-Type must be application/json\"}" :
                        policy == 403 ? "{\"error\":\"foreign origin\"}" : "{\"error\":\"chunked bodies not supported\"}");
                    return;
                }
                if (request->contentLength() > ApiExchange::kMaxBodySize) {
                    sendJson(request, 413, "{\"error\":\"request body too large\"}");
                    return;
                }
                const int bodyStatus = web::bodyStatus(static_cast<const web::BodyReceipt*>(request->_tempObject),
                    request->contentLength(), request->getAttribute(kBodyAllocationFailed, false));
                if (bodyStatus != 200) {
                    sendJson(request, bodyStatus, bodyStatus == 500 ? "{\"error\":\"body allocation failed\"}" :
                        "{\"error\":\"incomplete or invalid body\"}");
                    return;
                }
                const ApiResponse response = api_.submit(name, bodyOf(request));
                sendJson(request, response.status, response.body);
            },
            nullptr, collectBody);
    }

    impl_->socket.onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*,
                                 std::uint8_t*, std::size_t) {
        // Clients only listen; requests go through the REST API.
        if (type == WS_EVT_CONNECT) client->text(api_.snapshot(ApiExchange::Snapshot::Status).c_str());
    });
    impl_->socket.setFilter([](AsyncWebServerRequest* request) {
        return !request->hasHeader("Origin") ||
            web::trustedOrigin(request->getHeader("Origin")->value().c_str(), WiFi.localIP().toString().c_str(),
                               WiFi.softAPIP().toString().c_str());
    });
    server.addHandler(&impl_->socket);

    // Captive portal: OS connectivity checks and unknown hosts land on the UI.
    const auto redirect = [](AsyncWebServerRequest* request) {
        request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
    };
    server.on("/generate_204", HTTP_GET, redirect);
    server.on("/hotspot-detect.html", HTTP_GET, redirect);
    server.on("/connecttest.txt", HTTP_GET, redirect);
    server.on("/ncsi.txt", HTTP_GET, redirect);
    server.onNotFound([](AsyncWebServerRequest* request) {
        const String host = request->host();
        const bool foreignHost = host != WiFi.softAPIP().toString() && host != WiFi.localIP().toString() &&
                                 host != String(kHostname) + ".local";
        if (request->method() == HTTP_GET && foreignHost && (WiFi.getMode() & WIFI_AP)) {
            request->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
            return;
        }
        sendJson(request, 404, "{\"error\":\"not found\"}");
    });
    server.begin();

    impl_->mdnsRunning = MDNS.begin(kHostname);
    if (impl_->mdnsRunning) MDNS.addService("http", "tcp", 80);
}

void WebPortal::applyMode(WifiMode mode) {
    const bool wantAp = mode == WifiMode::Ap || mode == WifiMode::ApSta;
    const bool wantSta = mode == WifiMode::Sta || mode == WifiMode::ApSta;
    WiFi.mode(wantAp && wantSta ? WIFI_AP_STA : wantAp ? WIFI_AP : WIFI_STA);
    if (wantAp) {
        if (!WiFi.softAP(apSsid_.c_str(), apPassword_.c_str())) {
            Serial.println("[WiFi] access point start failed; retrying with default password");
            apPassword_ = BridgeConfig{}.apPassword;
            if (!WiFi.softAP(apSsid_.c_str(), apPassword_.c_str()))
                Serial.println("[WiFi] access point retry failed");
        }
        if (impl_ && !impl_->dnsRunning) {
            impl_->dns.setErrorReplyCode(DNSReplyCode::NoError);
            impl_->dnsRunning = impl_->dns.start(kDnsPort, "*", WiFi.softAPIP());
        }
        Serial.printf("[WiFi] access point \"%s\" (WPA2 password: %s) at http://%s/\n", apSsid_.c_str(),
                      apPassword_ == BridgeConfig{}.apPassword ? apPassword_.c_str() : "(custom)",
                      WiFi.softAPIP().toString().c_str());
    } else if (impl_ && impl_->dnsRunning) {
        impl_->dns.stop();
        impl_->dnsRunning = false;
    }
    if (wantSta && WiFi.status() != WL_CONNECTED) {
        WiFi.begin(ssid_.c_str(), password_.empty() ? nullptr : password_.c_str());
        WiFi.setAutoReconnect(true);
        Serial.printf("[WiFi] connecting to \"%s\"\n", ssid_.c_str());
    }
    applied_ = mode;
}

NetworkStatus WebPortal::networkStatus() const {
    NetworkStatus status;
    status.mode = wifiModeName(applied_);
    status.hostname = std::string(kHostname) + ".local";
    if (WiFi.status() == WL_CONNECTED) {
        status.ssid = ssid_;
        status.ip = WiFi.localIP().toString().c_str();
    } else if (applied_ != WifiMode::Sta) {
        status.ssid = apSsid_;
        status.ip = WiFi.softAPIP().toString().c_str();
    } else {
        status.ssid = ssid_;
    }
    if (applied_ != WifiMode::Sta) status.stations = WiFi.softAPgetStationNum();
    return status;
}

void WebPortal::publish(std::uint32_t now) {
    if (now - lastStatusMs_ >= kStatusIntervalMs) {
        lastStatusMs_ = now;
        ui_.setNetworkStatus(networkStatus());
        const std::string status = ui_.statusJson();
        api_.publish(ApiExchange::Snapshot::Status, status);
        impl_->broadcast(status);
    }
    if (now - lastScanMs_ >= kScanIntervalMs) {
        lastScanMs_ = now;
        std::string scan = ui_.scanResultsJson();
        if (scan != lastScanJson_) {
            api_.publish(ApiExchange::Snapshot::Scan, scan);
            lastScanJson_ = std::move(scan);
            impl_->scanPending = true;
        }
        std::string config = ui_.configJson();
        if (config != lastConfigJson_) {
            api_.publish(ApiExchange::Snapshot::Config, config);
            lastConfigJson_ = std::move(config);
            impl_->configPending = true;
        }
    }
    if (impl_->scanPending && impl_->broadcast(lastScanJson_)) impl_->scanPending = false;
    if (impl_->configPending && impl_->broadcast(lastConfigJson_)) impl_->configPending = false;
}

void WebPortal::loop() {
    if (!started_) return;
    const std::uint32_t now = millis();
    for (const std::string& message : api_.process(ui_)) {
        impl_->broadcast(message);  // Results remain available through GET /api/result if congested.
        lastStatusMs_ = now - kStatusIntervalMs;  // push the effect right away
        lastScanMs_ = now - kScanIntervalMs;
    }
    publish(now);
    if (impl_->dnsRunning) impl_->dns.processNextRequest();
    if (now - lastPolicyMs_ >= kPolicyIntervalMs) {
        lastPolicyMs_ = now;
        impl_->socket.cleanupClients();
        const int stations = applied_ != WifiMode::Sta ? WiFi.softAPgetStationNum() : 0;
        const WifiMode mode = policy_.update(WiFi.status() == WL_CONNECTED, stations, now);
        if (mode != applied_) applyMode(mode);
    }
}

#endif  // ARDUINO
