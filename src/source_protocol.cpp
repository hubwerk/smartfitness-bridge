#include "protocols/source_protocol.hpp"
#include "protocols/registry.hpp"

#include <cmath>

namespace smartfitness {
namespace protocols {

MachineProfile SourceProtocol::profile() const {
    const auto* info = findProtocol(id());
    return info ? info->profile : MachineProfile::IndoorBike;
}

void SourceProtocol::onRead(std::size_t, const std::uint8_t*, std::size_t) {}
void SourceProtocol::onAdvertisement(const ble::Advertisement&, std::uint32_t) {}
void SourceProtocol::onWriteFailed(std::uint32_t) { setError("write failed"); }
void SourceProtocol::tick(std::uint32_t) {}
bool SourceProtocol::control(const ControlRequest&, std::uint32_t) { return false; }

void SourceProtocol::reset() {
    sample_ = {};
    outbox_.clear();
    lastError_.clear();
    lastDataMs_ = 0;
    observedMetrics_ = 0;
    hasData_ = false;
    failed_ = false;
}

bool SourceProtocol::takeWrite(ble::GattWrite& out) {
    if (outbox_.empty()) return false;
    out = outbox_.front();
    outbox_.erase(outbox_.begin());
    return true;
}

bool SourceProtocol::queueWrite(const ble::Uuid& service, const ble::Uuid& characteristic, const ble::Bytes& payload,
                                bool withResponse) {
    if (outbox_.size() >= kMaxOutbox || payload.size() > ble::kMaxWriteLength) {
        setError("write queue full");
        return false;
    }
    outbox_.push_back(ble::GattWrite::make(service, characteristic, payload, withResponse));
    return true;
}

void SourceProtocol::touch(std::uint32_t nowMs) {
    hasData_ = true;
    lastDataMs_ = nowMs;
    const auto observe = [this](const MetricSample& sample, Metric metric) {
        if (sample.valid) observedMetrics_ |= 1U << static_cast<unsigned int>(metric);
    };
    observe(sample_.speed, Metric::Speed);
    observe(sample_.cadence, Metric::Cadence);
    observe(sample_.power, Metric::Power);
    observe(sample_.heartRate, Metric::HeartRate);
    observe(sample_.incline, Metric::Incline);
    observe(sample_.distance, Metric::Distance);
    observe(sample_.elapsed, Metric::ElapsedTime);
}

int percentToLevel(float percent, int minimum, int maximum) {
    if (!std::isfinite(percent)) percent = 0.0F;
    if (percent < 0.0F) percent = 0.0F;
    if (percent > 100.0F) percent = 100.0F;
    const float level = static_cast<float>(minimum) + percent / 100.0F * static_cast<float>(maximum - minimum);
    return static_cast<int>(std::lround(level));
}

}  // namespace protocols
}  // namespace smartfitness
