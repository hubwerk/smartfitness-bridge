#pragma once

// Vendor treadmill BLE protocols (TRUE/Assault/WDWAY, Life Fitness, LifeSpan,
// Octane Zero Runner, ZiPro, ESLinker, ShuaA5, Sperax, Focus, DeerRun PitPat,
// Activio, Technogym MyRun BLE, Wahoo KICKR RUN).
//
// GPL-3.0 port of qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift),
// Copyright Roberto Viola and contributors. Pinned revision
// ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/technogymmyruntreadmill/technogymmyruntreadmill.{cpp,h},
// src/devices/truetreadmill/truetreadmill.{cpp,h},
// src/devices/lifefitnesstreadmill/lifefitnesstreadmill.{cpp,h},
// src/devices/lifespantreadmill/lifespantreadmill.{cpp,h},
// src/devices/octanetreadmill/octanetreadmill.{cpp,h},
// src/devices/ziprotreadmill/ziprotreadmill.{cpp,h},
// src/devices/eslinkertreadmill/eslinkertreadmill.{cpp,h},
// src/devices/shuaa5treadmill/shuaa5treadmill.{cpp,h},
// src/devices/speraxtreadmill/speraxtreadmill.{cpp,h},
// src/devices/focustreadmill/focustreadmill.{cpp,h},
// src/devices/deeruntreadmill/deerruntreadmill.{cpp,h},
// src/devices/activiotreadmill/activiotreadmill.{cpp,h},
// src/devices/wahookickruntreadmill/wahookickruntreadmill.{cpp,h} and the
// discovery rules in src/devices/bluetooth.cpp. This port is part of
// smartfitness-bridge and distributed under GPL-3.0-or-later.
//
// Treadmill safety: the bridge never starts the belt. Upstream start frames
// (and init frames identical to them) are omitted, speed/incline requests are
// only accepted on fresh telemetry while the belt is already running, and a
// speed of 0 maps to the upstream stop frame only while the belt runs.

#include <cstddef>
#include <cstdint>

#include "protocols/ftms_source.hpp"
#include "protocols/registry.hpp"
#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace vendor_treadmills {

using Bytes = ble::Bytes;

constexpr int kMatchPriority = 80;
constexpr std::uint32_t kFreshDataMs = 2000;

enum class Model : std::uint8_t {
    True,
    Assault,
    Wdway,
    TechnogymMyRun,
    Lifespan,
    OctaneZr7,
    OctaneZr8,
    Zipro,
    EsangLinker,
    Costaway,
    Sperax,
    Focus,
    PitPat,
    Activio,
    ActivioV460,
    ActivioZt2500,
    ActivioUmay,
    KickrRun,
    Count
};

// Upstream discovery rules (bluetooth.cpp), exposed for tests.
bool matchesTrue(const ble::Advertisement& advertisement);
bool matchesAssault(const ble::Advertisement& advertisement);
bool matchesWdway(const ble::Advertisement& advertisement);
bool matchesLifeFitness(const ble::Advertisement& advertisement);
bool matchesShuaA5(const ble::Advertisement& advertisement);
bool matchesModel(Model model, const ble::Advertisement& advertisement);

// Frame helpers (exact upstream bytes), exposed for tests.
std::uint8_t crc8(const std::uint8_t* data, std::size_t length);  // poly 0x07, init 0
Bytes ziproPoll(bool force, std::uint8_t speedTenths, std::uint8_t incline);
Bytes focusControl(std::uint8_t speedTenths, std::uint8_t incline);
Bytes speraxSpeedFrame(int speedTenths);  // empty outside 1..60
Bytes pitpatControl(std::uint16_t speedThousandths, std::uint8_t incline);
Bytes activioSpeedFrame(std::uint8_t speedTenths, bool alternateChecksum);
Bytes activioInclineFrame(std::uint8_t incline, bool umay, std::uint8_t speedTenths);
Bytes esangSpeedFrame(float kmh);
Bytes costawaySpeedFrame(float kmh);
Bytes esangHandshakeReply(std::uint8_t random);
Bytes esangPairReply(std::uint8_t selector, std::uint8_t n1, std::uint8_t n2);
Bytes kickrRunSpeedFrame(float kmh, bool first);
int activioDecode(std::uint8_t raw);
int activioIncline(std::uint8_t raw);

std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace vendor_treadmills

class VendorTreadmillSource : public SourceProtocol {
public:
    explicit VendorTreadmillSource(vendor_treadmills::Model model);

    const char* id() const override;
    MachineProfile profile() const override { return MachineProfile::Treadmill; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onWriteEndpoint(const ble::Uuid& service, const ble::Uuid& characteristic) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void onWriteFailed(std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    vendor_treadmills::Model model() const { return model_; }
    bool ready() const { return ready_; }

private:
    void send(std::uint8_t target, const ble::Bytes& payload);
    void runScript(std::uint32_t nowMs);
    void poll(std::uint32_t nowMs);
    void decode(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs);
    void decodeOctane(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs);
    bool running(std::uint32_t nowMs) const;
    bool fresh(std::uint32_t nowMs) const;
    std::uint8_t nextRandom();

    vendor_treadmills::Model model_;
    bool connected_ = false;
    bool ready_ = false;
    bool haveEndpoint_ = false;
    ble::Uuid endpointService_{};
    std::uint8_t step_ = 0;
    std::uint8_t waitEvent_ = 0;  // 1 handshake, 2 pair
    std::uint8_t events_ = 0;     // bit0 handshake seen, bit1 pair seen
    std::uint32_t nextStepMs_ = 0;
    std::uint32_t nextPollMs_ = 0;
    std::uint8_t lastPoll_ = 0;   // lifespan: last queried value
    bool controlStarted_ = false;  // KICKR RUN workout mode unlocked
    bool ftmsRequested_ = false;   // KICKR RUN FTMS control requested
    bool firstSpeedSent_ = false;
    bool paddlePending_ = false;
    bool deviceIdSeen_ = false;
    std::uint32_t random_ = 0x2545F491U;
    // Octane ZR8 reassembly and cadence tracking.
    ble::Bytes buffer_;
    std::size_t expected_ = 0;
    std::uint32_t lowCadenceSinceMs_ = 0;
    std::uint32_t lastCadenceMs_ = 0;
};

// FTMS-data vendor treadmills (Life Fitness read-only, ShuaA5 controllable).
class VendorFtmsTreadmillSource : public FtmsSource {
public:
    explicit VendorFtmsTreadmillSource(const Options& options) : FtmsSource(options) {}
    MachineProfile profile() const override { return MachineProfile::Treadmill; }
    // Treadmill safety: controls only on fresh data while the belt runs.
    bool control(const ControlRequest& request, std::uint32_t nowMs) override {
        if (!hasData() || nowMs - lastDataMs() > vendor_treadmills::kFreshDataMs) return false;
        if (!sample().speed.valid || !(sample().speed.value > 0.0F)) return false;
        return FtmsSource::control(request, nowMs);
    }
};

}  // namespace protocols
}  // namespace smartfitness
