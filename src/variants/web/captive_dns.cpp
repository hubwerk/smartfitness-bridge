#include "captive_dns.h"

#include <algorithm>
#include <cctype>

namespace web {
namespace {

constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kMaxName = 255;
constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypeAny = 255;
constexpr std::uint16_t kClassIn = 1;
constexpr std::uint8_t kRcodeNxDomain = 3;
constexpr std::uint32_t kTtlSeconds = 60;

// Plain-HTTP connectivity probes that make operating systems open their
// captive-portal sign-in (port 80 then redirects to HTTPS).
constexpr const char* kProbeHosts[] = {
    "connectivitycheck.gstatic.com", "connectivitycheck.android.com", "clients1.google.com",
    "clients3.google.com",           "captive.apple.com",             "www.apple.com",
    "www.appleiphonecell.com",       "www.msftconnecttest.com",       "www.msftncsi.com",
    "detectportal.firefox.com",      "nmcheck.gnome.org",             "network-test.debian.org",
    "connectivity-check.ubuntu.com",
};

std::uint16_t read16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

bool CaptiveDns::resolves(const std::string& name) const {
    if (answerAll) return true;
    std::string host = lower(name);
    if (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty()) return false;
    const std::string own = lower(hostname);
    if (!own.empty() && (host == own || host == own + ".local")) return true;
    return std::any_of(std::begin(kProbeHosts), std::end(kProbeHosts),
                       [&host](const char* probe) { return host == probe; });
}

bool CaptiveDns::reply(const std::uint8_t* query, std::size_t size, std::vector<std::uint8_t>& response) const {
    response.clear();
    if (!query || size < kHeaderBytes + 5) return false;
    const std::uint16_t flags = read16(query + 2);
    const bool isResponse = flags & 0x8000;
    const unsigned opcode = (flags >> 11) & 0x0f;
    if (isResponse || opcode != 0 || read16(query + 4) != 1 || read16(query + 6) != 0 || read16(query + 8) != 0)
        return false;

    // QNAME: uncompressed labels up to the root label.
    std::string name;
    std::size_t offset = kHeaderBytes;
    while (true) {
        if (offset >= size) return false;
        const std::uint8_t length = query[offset++];
        if (length == 0) break;
        if (length > 63 || offset + length > size || name.size() + length + 1 > kMaxName) return false;
        if (!name.empty()) name.push_back('.');
        name.append(reinterpret_cast<const char*>(query + offset), length);
        offset += length;
    }
    if (offset + 4 > size) return false;
    const std::uint16_t type = read16(query + offset);
    const std::uint16_t klass = read16(query + offset + 2);
    const std::size_t questionEnd = offset + 4;

    const bool known = resolves(name);
    const bool answer = known && klass == kClassIn && (type == kTypeA || type == kTypeAny);
    // Known names without an IPv4 answer (AAAA, ...) get an empty NOERROR reply.
    const std::uint8_t rcode = known ? 0 : kRcodeNxDomain;

    response.reserve(questionEnd + 16);
    response.insert(response.end(), query, query + 2);  // ID
    // QR, opcode 0, AA, RD copied from the query; RA 0.
    response.push_back(static_cast<std::uint8_t>(0x84 | (query[2] & 0x01)));
    response.push_back(rcode);
    put16(response, 1);
    put16(response, answer ? 1 : 0);
    put16(response, 0);
    put16(response, 0);
    // The question is echoed byte for byte (keeps DNS 0x20 case randomization).
    response.insert(response.end(), query + kHeaderBytes, query + questionEnd);
    if (answer) {
        put16(response, 0xc000 | kHeaderBytes);  // pointer to the question name
        put16(response, kTypeA);
        put16(response, kClassIn);
        put16(response, static_cast<std::uint16_t>(kTtlSeconds >> 16));
        put16(response, static_cast<std::uint16_t>(kTtlSeconds));
        put16(response, static_cast<std::uint16_t>(address.size()));
        response.insert(response.end(), address.begin(), address.end());
    }
    return true;
}

}  // namespace web
