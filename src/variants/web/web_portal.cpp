#ifdef ARDUINO

#include "web_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_https_server.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "generated/web_assets.h"
#include "config_store.h"
#include "tls_cert.h"
#include "web_request.h"

namespace {
constexpr std::uint16_t kDnsPort = 53;
constexpr std::uint32_t kPolicyIntervalMs = 1000;
// Each TLS session holds ~35 kB of internal RAM (mbedTLS record buffers).
constexpr std::size_t kMaxHttpsSockets = 4;
constexpr std::size_t kMaxRedirectSockets = 2;
constexpr std::size_t kMaxPlainSockets = 7;  // HTTPS off: UI/API on port 80
constexpr std::size_t kMaxClientSockets = kMaxPlainSockets;
constexpr std::uint16_t kHttpsCtrlPort = 32768;
constexpr std::uint16_t kHttpCtrlPort = 32769;
constexpr std::size_t kMaxSocketFrame = 128;  // clients only listen; anything larger closes the socket
constexpr int kBodyRecvRetries = 3;

const char* const kPostEndpoints[] = {"scan",     "pair",    "unpair", "connect",       "disconnect", "simulate",
                                      "control",  "config",  "factory-reset", "restart",    "wifi-scan"};

// Scheme of the web interface; written in WebPortal::begin() before any server task starts.
bool secureWeb = true;

const char* scheme() { return secureWeb ? "https://" : "http://"; }
std::string staIp() { return WiFi.localIP().toString().c_str(); }
std::string apIp() { return WiFi.softAPIP().toString().c_str(); }

// False if the header is absent; an empty header is present with an empty value.
bool header(httpd_req_t* request, const char* name, std::string& value) {
    value.clear();
    const std::size_t length = httpd_req_get_hdr_value_len(request, name);
    std::string buffer(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, name, &buffer[0], buffer.size()) != ESP_OK) return false;
    value.assign(buffer.c_str(), length);
    return true;
}

bool hasHeader(httpd_req_t* request, const char* name) {
    std::string ignored;
    return header(request, name, ignored);
}

std::string pathOf(httpd_req_t* request) {
    const char* uri = request->uri;
    return std::string(uri, std::strcspn(uri, "?"));
}

esp_err_t sendJson(httpd_req_t* request, int status, const std::string& body) {
    httpd_resp_set_status(request, web::statusLine(status));
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t redirectTo(httpd_req_t* request, const std::string& location) {
    httpd_resp_set_status(request, web::statusLine(302));
    httpd_resp_set_hdr(request, "Location", location.c_str());
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, nullptr, 0);
}

// Answers and, if the request body was not read, closes the connection
// instead of draining a body nobody wants.
esp_err_t reject(httpd_req_t* request, int status, const char* body) {
    sendJson(request, status, body);
    return request->content_len > 0 ? ESP_FAIL : ESP_OK;
}

// Reads exactly Content-Length bytes (max ApiExchange::kMaxBodySize).
int readBody(httpd_req_t* request, std::string& body) {
    const std::size_t total = request->content_len;
    std::string storage(total + 1, '\0');
    web::BodyReceipt receipt{total, 0, false};
    char chunk[256];
    int retries = 0;
    while (receipt.received < total && !receipt.invalid) {
        const std::size_t wanted = std::min(sizeof(chunk), total - receipt.received);
        const int got = httpd_req_recv(request, chunk, wanted);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++retries < kBodyRecvRetries) continue;
        if (got <= 0) break;
        receipt.append(&storage[0], chunk, static_cast<std::size_t>(got), receipt.received, total);
    }
    const int status = web::bodyStatus(&receipt, total, false);
    if (status == 200) body.assign(storage.c_str(), receipt.received);
    return status;
}

// Captive portal: OS connectivity checks and unknown hosts land on the UI.
esp_err_t notFound(httpd_req_t* request, httpd_err_code_t) {
    std::string host;
    header(request, "Host", host);
    if (request->method == HTTP_GET && !web::ownHost(host, staIp(), apIp()) && (WiFi.getMode() & WIFI_AP))
        return redirectTo(request, scheme() + apIp() + "/");
    return reject(request, 404, "{\"error\":\"not found\"}");
}

// HTTPS mode: port 80 only redirects to HTTPS (captive-portal probes included).
esp_err_t redirectToHttps(httpd_req_t* request) {
    std::string host;
    header(request, "Host", host);
    return redirectTo(request, web::httpsRedirect(host, request->uri, staIp(), apIp()));
}
// HTTPS off: OS connectivity checks land on the UI.
esp_err_t redirectToPortal(httpd_req_t* request) { return redirectTo(request, "http://" + apIp() + "/"); }
}  // namespace

struct WebPortal::Impl {
    struct Broadcast {
        Impl* impl;
        std::string message;
    };

    WebPortal& portal;
    httpd_handle_t server = nullptr;    // UI, REST API and WebSocket (HTTPS on 443, or HTTP on 80)
    httpd_handle_t redirect = nullptr;  // HTTPS mode only: port 80 -> HTTPS
    web::TlsIdentity tls;
    DNSServer dns;
    bool dnsRunning = false;
    bool mdnsRunning = false;
    bool scanPending = false;
    bool configPending = false;
    std::atomic<bool> sending{false};

    explicit Impl(WebPortal& owner) : portal(owner) {}

    static Impl& of(httpd_req_t* request) { return *static_cast<Impl*>(request->user_ctx); }

    static esp_err_t index(httpd_req_t* request) {
        httpd_resp_set_type(request, "text/html");
        httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
        return httpd_resp_send(request, reinterpret_cast<const char*>(kIndexHtmlGz), sizeof(kIndexHtmlGz));
    }

    static esp_err_t apiGet(httpd_req_t* request) {
        ApiExchange& api = of(request).portal.api_;
        const std::string path = pathOf(request);
        if (path == "/api/status") return sendJson(request, 200, api.snapshot(ApiExchange::Snapshot::Status));
        if (path == "/api/scan/results") return sendJson(request, 200, api.snapshot(ApiExchange::Snapshot::Scan));
        if (path == "/api/wifi-scan/results")
            return sendJson(request, 200, api.snapshot(ApiExchange::Snapshot::WifiScan));
        if (path == "/api/config") return sendJson(request, 200, api.snapshot(ApiExchange::Snapshot::Config));
        if (path == "/api/result") {
            char query[64] = {};
            char id[16] = {};
            const bool found = httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
                               httpd_query_key_value(query, "id", id, sizeof(id)) == ESP_OK;
            const ApiResponse response =
                api.result(found ? static_cast<std::uint32_t>(std::strtoul(id, nullptr, 10)) : 0);
            return sendJson(request, response.status, response.body);
        }
        return notFound(request, HTTPD_404_NOT_FOUND);
    }

    static esp_err_t apiPost(httpd_req_t* request) {
        WebPortal& portal = of(request).portal;
        const std::string name = pathOf(request).substr(std::strlen("/api/"));
        bool known = false;
        for (const char* endpoint : kPostEndpoints) known = known || name == endpoint;
        if (!known) return reject(request, 404, "{\"error\":\"not found\"}");

        std::string contentType;
        std::string origin;
        header(request, "Content-Type", contentType);
        const bool hasOrigin = header(request, "Origin", origin);
        const int policy = web::postRequestStatus(contentType, hasOrigin, origin, staIp(), apIp(),
                                                  hasHeader(request, "Transfer-Encoding"), secureWeb);
        if (policy != 200) {
            return reject(request, policy, policy == 415 ? "{\"error\":\"Content-Type must be application/json\"}" :
                policy == 403 ? "{\"error\":\"foreign origin\"}" : "{\"error\":\"chunked bodies not supported\"}");
        }
        if (request->content_len > ApiExchange::kMaxBodySize)
            return reject(request, 413, "{\"error\":\"request body too large\"}");
        std::string body;
        if (readBody(request, body) != 200) {
            sendJson(request, 400, "{\"error\":\"incomplete or invalid body\"}");
            return ESP_FAIL;  // the rest of the body is unknown; do not reuse the connection
        }
        if (name == "wifi-scan") {  // handled by the Wi-Fi manager, not BridgeController
            portal.wifiScanRequested_ = true;
            return sendJson(request, 202, "{\"scanning\":true}");
        }
        const ApiResponse response = portal.api_.submit(name, body);
        return sendJson(request, response.status, response.body);
    }

    // Server-to-client telemetry on wss://<host>/ws (ws:// with HTTPS off). Clients only listen; requests go through the REST API.
    static esp_err_t socket(httpd_req_t* request) {
        if (request->method == HTTP_GET) {  // called right after the handshake
            std::string origin;
            if (header(request, "Origin", origin) && !web::trustedOrigin(origin, staIp(), apIp(), secureWeb))
                return ESP_FAIL;  // closes the socket before anything is sent
            const std::string status = of(request).portal.api_.snapshot(ApiExchange::Snapshot::Status);
            httpd_ws_frame_t frame{};
            frame.final = true;
            frame.type = HTTPD_WS_TYPE_TEXT;
            frame.payload = reinterpret_cast<std::uint8_t*>(const_cast<char*>(status.data()));
            frame.len = status.size();
            return httpd_ws_send_frame(request, &frame);
        }
        httpd_ws_frame_t frame{};
        if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK || frame.len > kMaxSocketFrame) return ESP_FAIL;
        std::uint8_t ignored[kMaxSocketFrame];
        frame.payload = ignored;
        return frame.len ? httpd_ws_recv_frame(request, &frame, frame.len) : ESP_OK;
    }

    // Runs on the server task, so TLS sessions are never written concurrently.
    static void sendBroadcast(void* argument) {
        std::unique_ptr<Broadcast> job(static_cast<Broadcast*>(argument));
        httpd_handle_t server = job->impl->server;
        std::array<int, kMaxClientSockets> fds{};
        std::size_t count = fds.size();
        if (httpd_get_client_list(server, &count, fds.data()) == ESP_OK) {
            httpd_ws_frame_t frame{};
            frame.final = true;
            frame.type = HTTPD_WS_TYPE_TEXT;
            frame.payload = reinterpret_cast<std::uint8_t*>(&job->message[0]);
            frame.len = job->message.size();
            for (std::size_t i = 0; i < count; ++i) {
                if (httpd_ws_get_fd_info(server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
                if (httpd_ws_send_frame_async(server, fds[i], &frame) == ESP_OK)
                    httpd_sess_update_lru_counter(server, fds[i]);  // listeners never send; keep them off the LRU purge
                else
                    httpd_sess_trigger_close(server, fds[i]);
            }
        }
        job->impl->sending = false;
    }

    bool broadcast(const std::string& message) {
        if (!server) return false;
        std::array<int, kMaxClientSockets> fds{};
        std::size_t count = fds.size();
        if (httpd_get_client_list(server, &count, fds.data()) != ESP_OK) return false;
        std::size_t listeners = 0;
        for (std::size_t i = 0; i < count; ++i) listeners += httpd_ws_get_fd_info(server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET;
        // One broadcast in flight at a time; slow clients make the loop skip updates instead of queueing them.
        if (!web::canBroadcast(listeners, !sending)) return false;
        auto* job = new (std::nothrow) Broadcast{this, message};
        if (!job) return false;
        sending = true;
        if (httpd_queue_work(server, sendBroadcast, job) != ESP_OK) {
            delete job;
            sending = false;
            return false;
        }
        return true;
    }

    void registerHandlers() {
        const httpd_uri_t handlers[] = {
            {"/", HTTP_GET, index, this, false, false, nullptr},
            {"/ws", HTTP_GET, socket, this, true, false, nullptr},
            {"/api/*", HTTP_GET, apiGet, this, false, false, nullptr},
            {"/api/*", HTTP_POST, apiPost, this, false, false, nullptr},
        };
        for (const httpd_uri_t& handler : handlers) httpd_register_uri_handler(server, &handler);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, notFound);
    }

    bool startHttps() {
        if (!web::loadOrCreateTlsIdentity(std::string(kHostname) + ".local", {192, 168, 4, 1}, tls)) return false;
        httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
        config.cacert_pem = reinterpret_cast<const std::uint8_t*>(tls.certificatePem.c_str());
        config.cacert_len = tls.certificatePem.size() + 1;
        config.prvtkey_pem = reinterpret_cast<const std::uint8_t*>(tls.privateKeyPem.c_str());
        config.prvtkey_len = tls.privateKeyPem.size() + 1;
        config.port_secure = kHttpsPort;
        config.httpd.ctrl_port = kHttpsCtrlPort;
        config.httpd.max_open_sockets = kMaxHttpsSockets;
        config.httpd.lru_purge_enable = true;
        config.httpd.uri_match_fn = httpd_uri_match_wildcard;
        if (httpd_ssl_start(&server, &config) != ESP_OK) {
            server = nullptr;
            return false;
        }
        registerHandlers();
        return true;
    }

    bool startHttpRedirect() {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = kHttpPort;
        config.ctrl_port = kHttpCtrlPort;
        config.max_open_sockets = kMaxRedirectSockets;
        config.lru_purge_enable = true;
        config.uri_match_fn = httpd_uri_match_wildcard;
        if (httpd_start(&redirect, &config) != ESP_OK) {
            redirect = nullptr;
            return false;
        }
        const httpd_uri_t handlers[] = {
            {"/*", HTTP_GET, redirectToHttps, this, false, false, nullptr},
            {"/*", HTTP_HEAD, redirectToHttps, this, false, false, nullptr},
        };
        for (const httpd_uri_t& handler : handlers) httpd_register_uri_handler(redirect, &handler);
        return true;
    }

    // HTTPS off: the same UI/API/WebSocket over plain HTTP on port 80.
    bool startHttp() {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = kHttpPort;
        config.ctrl_port = kHttpCtrlPort;
        config.max_open_sockets = kMaxPlainSockets;
        config.max_uri_handlers = 12;
        config.lru_purge_enable = true;
        config.uri_match_fn = httpd_uri_match_wildcard;
        if (httpd_start(&server, &config) != ESP_OK) {
            server = nullptr;
            return false;
        }
        registerHandlers();
        for (const char* probe : {"/generate_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt"}) {
            const httpd_uri_t handler{probe, HTTP_GET, redirectToPortal, this, false, false, nullptr};
            httpd_register_uri_handler(server, &handler);
        }
        return true;
    }
};

WebPortal::WebPortal(BridgeController& ui, ApiExchange& api) : ui_(ui), api_(api) {}

void WebPortal::begin(const std::string& ssid, const std::string& password, const std::string& apPassword,
                      bool https) {
    if (started_) return;
    started_ = true;
    https_ = https;
    secureWeb = https;
    impl_ = new Impl(*this);
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
    api_.publish(ApiExchange::Snapshot::WifiScan, web::wifiScanJson(wifiScan_));

    if (https_ && !impl_->startHttps()) {
        // Keep the web interface reachable (e.g. to switch HTTPS off) rather than losing it.
        Serial.println("[Web] HTTPS server start failed; falling back to plain HTTP");
        https_ = false;
        secureWeb = false;
    }
    if (https_) {
        if (!impl_->startHttpRedirect()) Serial.println("[Web] HTTP redirect server start failed");
    } else if (!impl_->startHttp()) {
        Serial.println("[Web] HTTP server start failed; web UI unavailable");
    }

    impl_->mdnsRunning = MDNS.begin(kHostname);
    if (impl_->mdnsRunning) {
        if (https_) MDNS.addService("https", "tcp", kHttpsPort);
        else MDNS.addService("http", "tcp", kHttpPort);
    }
    Serial.printf("[Web] %s%s.local/ (or the address shown above)%s\n", https_ ? "https://" : "http://", kHostname,
                  https_ ? "; self-signed certificate" : "; HTTPS is off");
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
        Serial.printf("[WiFi] access point \"%s\" (WPA2 password: %s) at %s%s/\n", apSsid_.c_str(),
                      apPassword_ == BridgeConfig{}.apPassword ? apPassword_.c_str() : "(custom)",
                      https_ ? "https://" : "http://", WiFi.softAPIP().toString().c_str());
    } else if (impl_ && impl_->dnsRunning) {
        impl_->dns.stop();
        impl_->dnsRunning = false;
    }
    applied_ = mode;
    if (wantSta && !wifiScan_.scanning()) beginStation();
}

void WebPortal::beginStation() {
    if (WiFi.status() == WL_CONNECTED) return;
    WiFi.begin(ssid_.c_str(), password_.empty() ? nullptr : password_.c_str());
    WiFi.setAutoReconnect(true);
    Serial.printf("[WiFi] connecting to \"%s\"\n", ssid_.c_str());
}

void WebPortal::updateWifiScan(std::uint32_t now) {
    if (wifiScanRequested_.exchange(false) && wifiScan_.start(now)) {
        // A station that is still connecting blocks scans; resume it afterwards.
        if (applied_ != WifiMode::Ap && WiFi.status() != WL_CONNECTED) WiFi.disconnect();
        WiFi.scanDelete();
        lastWifiScanAttemptMs_ = now - kWifiScanRetryMs;
        api_.publish(ApiExchange::Snapshot::WifiScan, web::wifiScanJson(wifiScan_));
        Serial.println("[WiFi] network scan started");
    }
    if (!wifiScan_.scanning()) return;
    const std::int16_t found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) return;
    if (found > 0) {
        for (std::int16_t index = 0; index < found; ++index)
            wifiScan_.add({WiFi.SSID(index).c_str(), static_cast<int>(WiFi.RSSI(index)),
                           WiFi.encryptionType(index) != WIFI_AUTH_OPEN});
        api_.publish(ApiExchange::Snapshot::WifiScan, web::wifiScanJson(wifiScan_));
    }
    if (found >= 0) WiFi.scanDelete();
    if (wifiScan_.windowOpen(now)) {
        if (now - lastWifiScanAttemptMs_ >= kWifiScanRetryMs) {
            lastWifiScanAttemptMs_ = now;
            WiFi.scanNetworks(true);
        }
        return;
    }
    wifiScan_.finish();
    if (applied_ == WifiMode::Ap) WiFi.enableSTA(false);  // scanning switched the station interface on
    else beginStation();
    api_.publish(ApiExchange::Snapshot::WifiScan, web::wifiScanJson(wifiScan_));
    Serial.printf("[WiFi] network scan finished: %u network(s)\n", static_cast<unsigned>(wifiScan_.networks().size()));
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
        const std::string status = web::statusJson(ui_.statusView(), networkStatus());
        api_.publish(ApiExchange::Snapshot::Status, status);
        impl_->broadcast(status);
    }
    if (now - lastScanMs_ >= kScanIntervalMs) {
        lastScanMs_ = now;
        std::string scan = web::scanResultsJson(ui_.scanView());
        if (scan != lastScanJson_) {
            api_.publish(ApiExchange::Snapshot::Scan, scan);
            lastScanJson_ = std::move(scan);
            impl_->scanPending = true;
        }
        std::string config = web::configJson(ui_.configView());
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
    updateWifiScan(now);
    if (impl_->dnsRunning) impl_->dns.processNextRequest();
    if (now - lastPolicyMs_ >= kPolicyIntervalMs) {
        lastPolicyMs_ = now;
        const int stations = applied_ != WifiMode::Sta ? WiFi.softAPgetStationNum() : 0;
        const WifiMode mode = policy_.update(WiFi.status() == WL_CONNECTED, stations, now);
        if (mode != applied_) applyMode(mode);
    }
}

#endif  // ARDUINO
