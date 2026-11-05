#pragma once

// Captive-portal DNS responder (pure C++, host tested; WebPortal serves it on
// UDP port 53 of the SoftAP).
//
// With HTTPS off every name resolves to the SoftAP (classic captive portal).
// With HTTPS on only the bridge's own name and the HTTP connectivity probes of
// common operating systems resolve; everything else gets NXDOMAIN. Otherwise
// every background HTTPS connection of a joined phone ends at the TLS server,
// fails the certificate check (mbedtls -0x7780) and the handshake flood
// exhausts CPU and internal RAM.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace web {

struct CaptiveDns {
    std::string hostname;                // bridge host name without ".local"
    std::array<std::uint8_t, 4> address{};  // SoftAP IPv4 address
    bool answerAll = true;               // false: bridge name + OS probes only

    // Case-insensitive; a trailing dot is ignored.
    bool resolves(const std::string& name) const;
    // Builds the reply to one DNS query. Returns false (drop the packet) unless
    // it is a well-formed standard query with exactly one question.
    bool reply(const std::uint8_t* query, std::size_t size, std::vector<std::uint8_t>& response) const;
};

}  // namespace web
