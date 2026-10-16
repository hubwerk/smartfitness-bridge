#include "service_names.h"

namespace smartfitness {

std::string serviceShortName(const ble::Uuid& uuid) {
    if (uuid.isShort()) {
        switch (uuid.shortValue()) {
            case 0x1826: return "FTMS";
            case 0x1818: return "CPS";
            case 0x1816: return "CSC";
            case 0x180d: return "HR";
            case 0x1814: return "RSC";
            case 0x180f: return "BAT";
            case 0x180a: return "DIS";
            default: return uuid.toString().substr(2);
        }
    }
    if (uuid == ble::Uuid::fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")) return "NUS";
    return uuid.toCanonicalString().substr(0, 8) + "\xe2\x80\xa6";
}

std::string serviceShortName(const std::string& value) {
    ble::Uuid uuid;
    return ble::Uuid::parse(value, uuid) ? serviceShortName(uuid) : value;
}

}  // namespace smartfitness
