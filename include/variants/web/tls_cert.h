#pragma once

// Self-signed TLS identity of the HTTPS web UI.
//  * ECDSA P-256 key and X.509 v3 server certificate, generated on the device
//    (hardware RNG) on first boot, so every bridge has its own key.
//  * Valid from 2025-01-01 until 9999-12-31 23:59:59 UTC, the RFC 5280 value
//    for "no well-defined expiration date": the certificate never expires.
//  * Subject CN and SAN: smartfitness.local, plus the SoftAP address as SAN IP.
//  * Firmware: stored in NVS namespace "sftls", separate from the configuration
//    blob, so factory resets and firmware updates keep the certificate (and the
//    browser exception for it). An unreadable or mismatching pair is replaced.

#include <array>
#include <cstdint>
#include <string>

namespace web {

struct TlsIdentity {
    std::string certificatePem;  // NUL-terminated PEM (esp_tls needs length incl. '\0')
    std::string privateKeyPem;
};

constexpr const char* kTlsNotBefore = "20250101000000";
constexpr const char* kTlsNotAfter = "99991231235959";

// Generates a new key pair and self-signed certificate. Pure mbedTLS.
bool generateTlsIdentity(const std::string& dnsName, const std::array<std::uint8_t, 4>& ipAddress,
                         TlsIdentity& identity, std::string& error);
// True if both PEMs parse and the key belongs to the certificate.
bool validTlsIdentity(const TlsIdentity& identity);

#ifdef ARDUINO
// Loads the stored identity or generates and stores a new one.
bool loadOrCreateTlsIdentity(const std::string& dnsName, const std::array<std::uint8_t, 4>& ipAddress,
                             TlsIdentity& identity);
#endif

}  // namespace web
