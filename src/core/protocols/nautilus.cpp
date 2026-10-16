// GPL-3.0 port of qdomyos-zwift, Copyright Roberto Viola and contributors.
// Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/{nautilusbike,nautiluselliptical,nautilustreadmill,bowflextreadmill}/
// (same-named .cpp/.h); src/devices/bluetooth.cpp (advertisement rules).
// Revision b1824db5379891d571a7ef2e219d20e6c7ec2c09 of
// src/devices/bluetooth.cpp and src/devices/nautilustreadmill/nautilustreadmill.cpp:
// "SCHWINN T"/"SCHWINN 570T" treadmill names and the 688a15a0 service.

#include "protocols/nautilus.hpp"

#include <cctype>
#include <memory>
#include <string>

namespace smartfitness {
namespace protocols {
namespace nautilus {
namespace {

constexpr const char* kWrite = "1717b3c0-9803-11e3-90e1-0002a5d5c51b";
constexpr const char* kNotify[] = {"a46a4a80-9803-11e3-8f3c-0002a5d5c51b", "6be8f580-9803-11e3-ab03-0002a5d5c51b"};
// bowflextreadmill subscribes to five characteristics (notify1..5 upstream).
constexpr const char* kBowflexNotify[] = {
    "35ddd0a0-9803-11e3-9a8b-0002a5d5c51b", "6be8f580-9803-11e3-ab03-0002a5d5c51b",
    "a46a4a80-9803-11e3-8f3c-0002a5d5c51b", "b8066ec0-9803-11e3-8346-0002a5d5c51b",
    "d57cda20-9803-11e3-8426-0002a5d5c51b"};
constexpr float kMile = 1.60934F;

struct Service {
    const char* uuid;
    Layout layout;
};

// Upstream serviceScanDone() fallback order.
constexpr Service kBikeServices[] = {{"b1f93401-d4bd-4839-b7d7-b7434e9656cc", Layout::BikeMiles},
                                     {"f755c9cf-e1fc-4ecd-8d90-f2d7ebf56b81", Layout::BikeB616},
                                     {"44f8d44f-7e03-4baf-9cc1-bd5a9c7a076b", Layout::BikeMiles}};
// B616 with upstream miles_unit=true: that service reports 0.01 mph too.
constexpr Service kBikeMilesServices[] = {{"b1f93401-d4bd-4839-b7d7-b7434e9656cc", Layout::BikeMiles},
                                          {"f755c9cf-e1fc-4ecd-8d90-f2d7ebf56b81", Layout::BikeMiles},
                                          {"44f8d44f-7e03-4baf-9cc1-bd5a9c7a076b", Layout::BikeMiles}};
// The fifth upstream fallback (7df7a3f7, variant 2) would exceed the
// 8-subscription plan limit and is a separate manual descriptor.
constexpr Service kEllipticalServices[] = {{"3a1a1d3f-3d83-4c43-aa7b-81664ed75ec8", Layout::EllipticalV0},
                                           {"ac8f2400-9804-11e3-b25b-0002a5d5c51b", Layout::EllipticalV1},
                                           {"b6492080-7f04-11e4-a8b1-0002a5d5c51b", Layout::EllipticalV1},
                                           {"219fbae0-9df6-11e5-a87d-0002a5d5c51b", Layout::EllipticalV1}};
constexpr Service kEllipticalV2Services[] = {{"7df7a3f7-f013-492f-a58e-68a8f078ab96", Layout::EllipticalV2}};
constexpr Service kTreadmillServices[] = {{"89e14b5e-a841-48ad-b580-935f4545fffc", Layout::Treadmill},
                                          {"f12f71ab-c228-45f2-98a2-6654f9adbe78", Layout::Treadmill},
                                          {"19df5b20-31c7-11e6-a336-0002a5d5c51b", Layout::Treadmill},
                                          {"4b2387a0-be8e-11e3-8039-0002a5d5c51b", Layout::Treadmill}};
// Added at b1824db before 4b2387a0; manual descriptor (plan limit).
constexpr Service kTreadmill688aServices[] = {{"688a15a0-3a30-11e6-b3f8-0002a5d5c51b", Layout::Treadmill}};
constexpr Service kBowflexServices[] = {{"edff9e80-cad7-11e5-ab63-0002a5d5c51b", Layout::Bowflex}};

struct Model {
    const Service* services;
    std::uint8_t serviceCount;
    bool m7;
    bool t628;
};

template <std::size_t N>
constexpr Model model(const Service (&services)[N], bool m7 = false, bool t628 = false) {
    return {services, static_cast<std::uint8_t>(N), m7, t628};
}

// Same order as kDescriptors (persisted ids: append only).
constexpr Model kModels[] = {
    model(kBikeServices),        model(kEllipticalServices), model(kEllipticalServices, true),
    model(kTreadmillServices),   model(kTreadmillServices, false, true),
    model(kEllipticalV2Services), model(kTreadmill688aServices), model(kBowflexServices),
    model(kBikeMilesServices)};

std::string upper(const std::string& text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool prefix(const std::string& text, const char* start) { return text.rfind(start, 0) == 0; }

// Upstream matches these names with toUpper(); FTMS advertisers are left to
// the generic FTMS adapter.
std::string candidate(const ble::Advertisement& a) { return a.hasService(0x1826) ? std::string() : upper(a.name); }

bool manualOnly(const ble::Advertisement&) { return false; }

std::uint16_t u16(const std::uint8_t* data, std::size_t low) {
    return static_cast<std::uint16_t>(data[low] | (data[low + 1] << 8));
}

void set(MetricSample& sample, float value) {
    sample.value = value;
    sample.valid = true;
}

// Upstream reads several bytes through signed QByteArray::at(); negative
// values would be nonsense levels, so they are dropped instead.
void setSigned(MetricSample& sample, std::uint8_t byte) {
    if (byte < 0x80) set(sample, byte);
}

template <std::size_t N>
std::unique_ptr<SourceProtocol> create() {
    return std::unique_ptr<SourceProtocol>(new NautilusSource(N));
}

const ProtocolInfo kDescriptors[] = {
    {"nautilus-bike", "Nautilus bike (B628)", kMatchPriority, matchesBike, create<0>, false, false,
     MachineProfile::IndoorBike},
    {"nautilus-ellip", "Nautilus elliptical (E616/E618)", kMatchPriority, matchesElliptical, create<1>, false, false,
     MachineProfile::CrossTrainer},
    {"nautilus-m7", "Nautilus M7 elliptical", kMatchPriority, matchesM7, create<2>, false, false,
     MachineProfile::CrossTrainer},
    {"nautilus-tm", "Nautilus / Schwinn treadmill", kMatchPriority, matchesTreadmill, create<3>, false, false,
     MachineProfile::Treadmill},
    {"nautilus-t628", "Nautilus T628 treadmill", kMatchPriority, matchesT628, create<4>, false, false,
     MachineProfile::Treadmill},
    {"nautilus-el-v2", "Nautilus elliptical (7df7a3f7 service)", 0, manualOnly, create<5>, false, false,
     MachineProfile::CrossTrainer},
    {"nautilus-tm-688a", "Nautilus treadmill (688a15a0 service)", 0, manualOnly, create<6>, false, false,
     MachineProfile::Treadmill},
    {"bowflex-tm", "Bowflex treadmill (bowflextreadmill)", 0, manualOnly, create<7>, false, false,
     MachineProfile::Treadmill},
    {"nautilus-b616-mi", "Nautilus B616 bike (mph console)", 0, manualOnly, create<8>, false, false,
     MachineProfile::IndoorBike}};

static_assert(sizeof(kDescriptors) / sizeof(kDescriptors[0]) == sizeof(kModels) / sizeof(kModels[0]),
              "descriptor/model tables must stay aligned");

bool bowflex(const Model& m) { return m.services[0].layout == Layout::Bowflex; }

}  // namespace

bool Reading::any() const {
    return speed.valid || cadence.valid || power.valid || heartRate.valid || resistance.valid || incline.valid ||
           totalEnergy.valid;
}

Bytes initFrame() { return {0x07, 0x01, 0xd3, 0x00, 0x1f, 0x05, 0x01}; }

bool decode(const Variant& variant, const std::uint8_t* data, std::size_t length, Reading& out) {
    out = {};
    if (!data) return false;
    switch (variant.layout) {
        case Layout::BikeMiles:
        case Layout::BikeB616:
            if (length == 20) {
                if (data[16] != 0) set(out.heartRate, data[16]);
                setSigned(out.resistance, data[18]);
                return true;
            }
            if (length != 14) return false;
            set(out.speed, u16(data, 3) / 100.0F * (variant.layout == Layout::BikeMiles ? kMile : 1.0F));
            set(out.power, u16(data, 6) / 100.0F);
            setSigned(out.cadence, data[1]);
            return true;
        case Layout::EllipticalV0:
        case Layout::EllipticalV1:
        case Layout::EllipticalV2: {
            const bool v1 = variant.layout == Layout::EllipticalV1;
            if (length == 20) {
                if (!variant.m7 && data[16] != 0) set(out.heartRate, data[16]);
                if (v1 && variant.m7) {
                    // M7 splits live data: 0x31 status/HR/resistance, 0x5e speed/cadence/power.
                    if (data[1] == 0x31) {
                        if (data[17] != 0) set(out.heartRate, data[17]);
                        set(out.resistance, data[19]);
                        return true;
                    }
                    if (data[1] != 0x5e) return false;
                    set(out.speed, u16(data, 2) / 100.0F);
                    set(out.power, u16(data, 7) / 100.0F);
                    set(out.cadence, data[5]);
                    return true;
                }
                set(out.resistance, data[18]);
                return true;
            }
            if (length != (v1 ? 12U : 14U)) return false;
            if (v1) {
                set(out.speed, u16(data, 7) / 10.0F);
            } else {
                set(out.speed, u16(data, 3) / 100.0F * (variant.layout == Layout::EllipticalV2 ? kMile : 1.0F));
            }
            set(out.cadence, variant.layout == Layout::EllipticalV2 ? data[1] : data[5]);
            return true;
        }
        case Layout::Treadmill:
            if (length != 12) return false;
            set(out.speed, u16(data, 3) * kMile / 100.0F);
            if (!variant.t628) setSigned(out.incline, data[6]);
            return true;
        case Layout::Bowflex:
            if (length != 17) return false;
            set(out.speed, data[14] / 10.0F);
            setSigned(out.incline, data[11]);
            // Upstream (at(7) << 8) | at(8) sign-extends byte 8; only the
            // unambiguous non-negative case is forwarded.
            if (data[8] < 0x80) set(out.totalEnergy, static_cast<float>((data[7] << 8) | data[8]));
            return true;
    }
    return false;
}

bool matchesBike(const ble::Advertisement& a) { return prefix(candidate(a), "NAUTILUS B"); }

bool matchesElliptical(const ble::Advertisement& a) {
    const auto name = candidate(a);
    return (prefix(name, "NAUTILUS E") || prefix(name, "NAUTILUS M") || prefix(name, "NAUTILUS 616")) &&
           !prefix(name, "NAUTILUS M7");
}

bool matchesM7(const ble::Advertisement& a) { return prefix(candidate(a), "NAUTILUS M7"); }

bool matchesTreadmill(const ble::Advertisement& a) {
    const auto name = candidate(a);
    return (prefix(name, "NAUTILUS T") && !prefix(name, "NAUTILUS T628")) || prefix(name, "SCHWINN T") ||
           prefix(name, "SCHWINN 570T");
}

bool matchesT628(const ble::Advertisement& a) { return prefix(candidate(a), "NAUTILUS T628"); }

std::size_t descriptorCount() { return sizeof(kDescriptors) / sizeof(kDescriptors[0]); }

const ProtocolInfo& descriptorAt(std::size_t index) {
    return kDescriptors[index < descriptorCount() ? index : 0];
}

}  // namespace nautilus

using nautilus::kModels;

NautilusSource::NautilusSource(std::size_t model) : model_(model < nautilus::descriptorCount() ? model : 0) {}

const char* NautilusSource::id() const { return nautilus::descriptorAt(model_).id; }

MachineProfile NautilusSource::profile() const { return nautilus::descriptorAt(model_).profile; }

nautilus::Layout NautilusSource::layout() const {
    const auto& m = kModels[model_];
    return m.services[service_ >= 0 ? service_ : 0].layout;
}

ble::ConnectPlan NautilusSource::plan() const {
    ble::ConnectPlan result;
    const auto& m = kModels[model_];
    for (std::uint8_t s = 0; s < m.serviceCount; ++s) {
        const auto service = ble::Uuid::fromString(m.services[s].uuid);
        if (nautilus::bowflex(m)) {
            for (const char* notify : nautilus::kBowflexNotify) result.subscribe(service, ble::Uuid::fromString(notify));
        } else {
            for (const char* notify : nautilus::kNotify) result.subscribe(service, ble::Uuid::fromString(notify));
        }
    }
    return result;
}

bool NautilusSource::onConnected(std::uint32_t subscribedMask, std::uint32_t) {
    reset();
    const auto& m = kModels[model_];
    const std::size_t perService = nautilus::bowflex(m) ? 5 : 2;
    for (std::uint8_t s = 0; s < m.serviceCount && service_ < 0; ++s) {
        const std::uint32_t bits = ((1U << perService) - 1U) << (s * perService);
        if (subscribedMask & bits) service_ = s;
    }
    if (service_ < 0) {
        setError("Nautilus telemetry service unavailable");
        return false;
    }
    // Upstream btinit() runs once the CCCDs are written; no reply is awaited.
    return queueWrite(ble::Uuid::fromString(m.services[service_].uuid), ble::Uuid::fromString(nautilus::kWrite),
                      nautilus::initFrame());
}

void NautilusSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (service_ < 0) return;
    const auto& m = kModels[model_];
    const std::size_t perService = nautilus::bowflex(m) ? 5 : 2;
    // Upstream only listens to the selected service object.
    if (index / perService != static_cast<std::size_t>(service_)) return;
    nautilus::Reading reading;
    if (!nautilus::decode({layout(), m.m7, m.t628}, data, length, reading) || !reading.any()) return;
    const MetricSample* fields[] = {&reading.speed,      &reading.cadence, &reading.power,      &reading.heartRate,
                                    &reading.resistance, &reading.incline, &reading.totalEnergy};
    MetricSample* targets[] = {&sample_.speed,      &sample_.cadence, &sample_.power,      &sample_.heartRate,
                               &sample_.resistance, &sample_.incline, &sample_.totalEnergy};
    for (std::size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        if (fields[i]->valid) *targets[i] = *fields[i];
    touch(nowMs);
}

TrainerCapabilities NautilusSource::capabilities() const {
    TrainerCapabilities capabilities;
    capabilities.metrics = observedMetrics();
    return capabilities;
}

void NautilusSource::reset() {
    SourceProtocol::reset();
    service_ = -1;
}

}  // namespace protocols
}  // namespace smartfitness
