#include "protocols/registry.hpp"

#include <cctype>

#include "ftms/ftms_codec.hpp"
#include "protocols/domyos.hpp"
#include "protocols/echelon.hpp"
#include "protocols/ftms_source.hpp"
#include "protocols/sensor_sources.hpp"
#include "protocols/yesoul_keiser.hpp"

namespace smartfitness {
namespace protocols {
namespace {

bool startsWith(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

std::string upper(const std::string& value) {
    std::string out = value;
    for (char& character : out) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return out;
}

bool hasFtms(const ble::Advertisement& advertisement) { return advertisement.hasService(ftms::kServiceUuid); }

// Name rules follow qdomyos-zwift's bluetooth.cpp device discovery.
bool matchesSchwinn(const ble::Advertisement& advertisement) {
    const std::string name = upper(advertisement.name);
    return startsWith(name, "IC BIKE") || (startsWith(name, "C7-") && advertisement.name.size() != 17) ||
           startsWith(name, "C9/C10");
}

bool matchesEchelon(const ble::Advertisement& advertisement) {
    const std::string name = upper(advertisement.name);
    // ECH-ROW / ECH-STRIDE / ECH-SC are rower, stride and stair climber
    // protocols that are not ported; FTMS-capable models use the FTMS adapter.
    return startsWith(advertisement.name, "ECH") && !startsWith(name, "ECH-ROW") &&
           !startsWith(name, "ECH-STRIDE") && !startsWith(name, "ECH-SC") && !hasFtms(advertisement);
}

bool matchesDomyos(const ble::Advertisement& advertisement) {
    return startsWith(advertisement.name, "Domyos-Bike") && !startsWith(advertisement.name, "DomyosBridge") &&
           !hasFtms(advertisement);
}

bool matchesYesoul(const ble::Advertisement& advertisement) {
    if (startsWith(advertisement.name, "YESOUL") || startsWith(upper(advertisement.name), "YS_G1M_")) return true;
    // Manufacturer 637 (0x027d, little endian in the payload) followed by 01 05 00 ff ff.
    const auto& data = advertisement.manufacturerData;
    return data.size() >= 7 && data[0] == 0x7d && data[1] == 0x02 && data[2] == 0x01 && data[3] == 0x05 &&
           data[4] == 0x00 && data[5] == 0xff && data[6] == 0xff;
}

bool matchesKeiser(const ble::Advertisement& advertisement) {
    if (!startsWith(advertisement.name, "M3")) return false;
    keiser::Broadcast broadcast;
    return keiser::parse(advertisement.manufacturerData.data(), advertisement.manufacturerData.size(), broadcast);
}

bool matchesFtms(const ble::Advertisement& advertisement) { return hasFtms(advertisement); }
bool matchesCps(const ble::Advertisement& advertisement) {
    return advertisement.hasService(sensors::kCyclingPowerService);
}
bool matchesCsc(const ble::Advertisement& advertisement) { return advertisement.hasService(sensors::kCscService); }
bool matchesHrs(const ble::Advertisement& advertisement) {
    return advertisement.hasService(sensors::kHeartRateService);
}

template <typename T>
std::unique_ptr<SourceProtocol> make() {
    return std::unique_ptr<SourceProtocol>(new T());
}

std::unique_ptr<SourceProtocol> makeSchwinn() {
    FtmsSource::Options options;
    options.id = "schwinn-ic4";
    options.allowControl = false;
    options.bikeDataOnly = true;
    return std::unique_ptr<SourceProtocol>(new FtmsSource(options));
}

const ProtocolInfo kProtocols[] = {
    {"keiser-m3i", "Keiser M3i (broadcast)", 90, matchesKeiser, make<KeiserM3iSource>, false, true},
    {"echelon", "Echelon Connect Sport", 80, matchesEchelon, make<EchelonSource>, true, false},
    {"domyos", "Domyos / Decathlon bike", 80, matchesDomyos, make<DomyosSource>, true, false},
    {"schwinn-ic4", "Schwinn IC4 / IC8", 70, matchesSchwinn, makeSchwinn, false, false},
    {"yesoul", "Yesoul bike", 70, matchesYesoul, make<YesoulSource>, false, false},
    {"ftms", "FTMS fitness machine", 50, matchesFtms, make<FtmsSource>, true, false},
    {"cps", "Cycling Power sensor", 40, matchesCps, make<CyclingPowerSource>, false, false},
    {"csc", "Speed/Cadence sensor", 30, matchesCsc, make<CscSource>, false, false},
    {"hrs", "Heart rate monitor", 20, matchesHrs, make<HeartRateSource>, false, false},
};

}  // namespace

std::size_t protocolCount() { return sizeof(kProtocols) / sizeof(kProtocols[0]); }
const ProtocolInfo& protocolAt(std::size_t index) { return kProtocols[index < protocolCount() ? index : 0]; }

const ProtocolInfo* findProtocol(const std::string& id) {
    for (const auto& info : kProtocols) {
        if (id == info.id) return &info;
    }
    return nullptr;
}

const ProtocolInfo* detectProtocol(const ble::Advertisement& advertisement) {
    const ProtocolInfo* best = nullptr;
    for (const auto& info : kProtocols) {
        if ((!best || info.priority > best->priority) && info.matches(advertisement)) best = &info;
    }
    return best;
}

std::unique_ptr<SourceProtocol> createProtocol(const std::string& id) {
    const ProtocolInfo* info = findProtocol(id);
    return info ? info->create() : nullptr;
}

}  // namespace protocols
}  // namespace smartfitness
