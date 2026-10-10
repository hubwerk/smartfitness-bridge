#include "ble/ble_types.hpp"

#include <cctype>
#include <cstdio>

namespace smartfitness {
namespace ble {
namespace {

// Bluetooth base UUID 0000xxxx-0000-1000-8000-00805f9b34fb.
constexpr std::array<std::uint8_t, 16> kBaseUuid{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};

int hexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

}  // namespace

Uuid Uuid::from16(std::uint16_t shortUuid) {
    Uuid uuid;
    uuid.bytes = kBaseUuid;
    uuid.bytes[2] = static_cast<std::uint8_t>(shortUuid >> 8);
    uuid.bytes[3] = static_cast<std::uint8_t>(shortUuid & 0xFFU);
    return uuid;
}

bool Uuid::parse(const std::string& text, Uuid& out) {
    std::string value = text;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) value = value.substr(2);
    if (value.size() == 4) {
        unsigned int parsed = 0;
        for (char character : value) {
            const int digit = hexValue(character);
            if (digit < 0) return false;
            parsed = (parsed << 4) | static_cast<unsigned int>(digit);
        }
        out = from16(static_cast<std::uint16_t>(parsed));
        return true;
    }
    if (value.size() != 36) return false;
    Uuid parsed;
    std::size_t byteIndex = 0;
    for (std::size_t index = 0; index < value.size();) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
            ++index;
            continue;
        }
        const int high = hexValue(value[index]);
        const int low = index + 1 < value.size() ? hexValue(value[index + 1]) : -1;
        if (high < 0 || low < 0 || byteIndex >= parsed.bytes.size()) return false;
        parsed.bytes[byteIndex++] = static_cast<std::uint8_t>((high << 4) | low);
        index += 2;
    }
    if (byteIndex != parsed.bytes.size()) return false;
    out = parsed;
    return true;
}

Uuid Uuid::fromString(const char* text) {
    Uuid uuid;
    if (text) parse(text, uuid);
    return uuid;
}

bool Uuid::isShort() const {
    if (bytes[0] != 0 || bytes[1] != 0) return false;
    for (std::size_t index = 4; index < bytes.size(); ++index) {
        if (bytes[index] != kBaseUuid[index]) return false;
    }
    return true;
}

std::uint16_t Uuid::shortValue() const {
    return static_cast<std::uint16_t>((bytes[2] << 8) | bytes[3]);
}

std::string Uuid::toString() const {
    if (isShort()) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "0x%04x", static_cast<unsigned int>(shortValue()));
        return buffer;
    }
    return toCanonicalString();
}

std::string Uuid::toCanonicalString() const {
    std::string out;
    out.reserve(36);
    static const char* digits = "0123456789abcdef";
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) out += '-';
        out += digits[bytes[index] >> 4];
        out += digits[bytes[index] & 0x0F];
    }
    return out;
}

bool Uuid::isZero() const {
    for (auto byte : bytes) {
        if (byte != 0) return false;
    }
    return true;
}

bool BleAddress::parse(const std::string& text, BleAddress& out, std::uint8_t type) {
    if (text.size() != 17) return false;
    BleAddress parsed;
    for (std::size_t index = 0; index < 6; ++index) {
        const std::size_t offset = index * 3;
        const int high = hexValue(text[offset]);
        const int low = hexValue(text[offset + 1]);
        if (high < 0 || low < 0) return false;
        if (index < 5 && text[offset + 2] != ':') return false;
        parsed.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    if (type > 3) return false;
    parsed.type = type;
    out = parsed;
    return true;
}

std::string BleAddress::toString() const {
    char buffer[18];
    std::snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x", bytes[0], bytes[1], bytes[2], bytes[3],
                  bytes[4], bytes[5]);
    return buffer;
}

bool BleAddress::isZero() const {
    for (auto byte : bytes) {
        if (byte != 0) return false;
    }
    return true;
}

void ScanReport::setName(const std::string& value) {
    nameLength = 0;
    for (char character : value) {
        if (nameLength == name.size()) break;
        // Keep the name printable so it can be shown and persisted safely.
        name[nameLength++] = (character < 0x20 || character > 0x7E) ? '?' : character;
    }
}

std::string ScanReport::nameString() const { return std::string(name.data(), nameLength); }

bool ScanReport::addService(const Uuid& uuid) {
    for (std::size_t index = 0; index < serviceCount; ++index) {
        if (services[index] == uuid) return true;
    }
    if (serviceCount == services.size()) return false;
    services[serviceCount++] = uuid;
    return true;
}

void ScanReport::setManufacturerData(const std::uint8_t* data, std::size_t length) {
    manufacturerLength = 0;
    if (!data) return;
    if (length > manufacturer.size()) length = manufacturer.size();
    for (std::size_t index = 0; index < length; ++index) manufacturer[index] = data[index];
    manufacturerLength = static_cast<std::uint8_t>(length);
}

Advertisement Advertisement::from(const ScanReport& report) {
    Advertisement advertisement;
    advertisement.address = report.address;
    advertisement.name = report.nameString();
    advertisement.rssi = report.rssi;
    advertisement.services.assign(report.services.begin(), report.services.begin() + report.serviceCount);
    advertisement.manufacturerData.assign(report.manufacturer.begin(),
                                          report.manufacturer.begin() + report.manufacturerLength);
    return advertisement;
}

bool Advertisement::hasService(const Uuid& uuid) const {
    for (const auto& service : services) {
        if (service == uuid) return true;
    }
    return false;
}

GattWrite GattWrite::make(const Uuid& service, const Uuid& characteristic, const Bytes& payload, bool withResponse) {
    GattWrite write;
    write.service = service;
    write.characteristic = characteristic;
    write.withResponse = withResponse;
    write.length = static_cast<std::uint8_t>(payload.size() > write.data.size() ? write.data.size() : payload.size());
    for (std::size_t index = 0; index < write.length; ++index) write.data[index] = payload[index];
    return write;
}

bool ConnectPlan::subscribe(const Uuid& service, const Uuid& characteristic, bool indicate) {
    if (subscriptionCount == subscriptions.size()) return false;
    subscriptions[subscriptionCount++] = {service, characteristic, indicate};
    return true;
}

bool ConnectPlan::read(const Uuid& service, const Uuid& characteristic) {
    if (readCount == reads.size()) return false;
    reads[readCount++] = {service, characteristic};
    return true;
}

}  // namespace ble
}  // namespace smartfitness
