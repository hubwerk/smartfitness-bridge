#pragma once

// BLE-only ports of cagnulein/qdomyos-zwift, GPL-3.0, Roberto Viola and
// contributors. Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3.
// Sources: src/devices/{proformbike,proformtreadmill,proformelliptical,
// nordictrackelliptical,proformellipticaltrainer,bowflext216treadmill,
// schwinn170bike}/<directory>.cpp; advertisement rules: src/devices/bluetooth.cpp.
// VeloCore is not identified by this pinned discovery or build manifest
// (src/qdomyos-zwift.pri): no verified BLE parser/init exists to port here.
// Peloton/DFC and WiFi/Telnet/ADB machine paths are intentionally excluded.

#include "protocols/registry.hpp"

namespace smartfitness {
namespace protocols {
namespace ifit_bowflex {

enum class Kind { Bike, Treadmill, FreeStride, Elliptical, EllipticalTrainer, BowflexT216, Schwinn170 };

extern const ble::Uuid kIfitService;
extern const ble::Uuid kIfitWrite;
extern const ble::Uuid kIfitNotify;

// Firmware-specific models are selected explicitly, never inferred from the
// generic I_* advertisement. The default is upstream's all-settings-off path.
// Keep Model references alive for the source lifetime; modelAt/findModel return
// static storage. Resistance maxima are capped to actual upstream commands.
struct Model {
    const char* name;
    Kind kind;
    const char* init;  // space-separated hexadecimal GATT writes
    const char* poll;
    int maximumResistance;
};
std::size_t modelCount();
const Model& modelAt(std::size_t index);
const Model* findModel(const std::string& name);
std::size_t descriptorCount();
const ProtocolInfo& descriptorAt(std::size_t index);

}  // namespace ifit_bowflex

class IfitBowflexSource final : public SourceProtocol {
public:
    explicit IfitBowflexSource(ifit_bowflex::Kind kind);
    explicit IfitBowflexSource(const ifit_bowflex::Model& model);
    // Schwinn170 upstream enumerates every service/characteristic dynamically.
    // The normal plan discovers notifications and the permitted upstream
    // write UUIDs. This constructor also permits a pre-discovered plan.
    IfitBowflexSource(const ifit_bowflex::Model& model, const ble::ConnectPlan& plan,
                     const ble::Uuid& writeService, const ble::Uuid& writeCharacteristic);
    const char* id() const override;
    ble::ConnectPlan plan() const override;
    std::uint32_t startupTimeoutMs() const override { return 35000; }
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onWriteEndpoint(const ble::Uuid& service, const ble::Uuid& characteristic) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void onWriteFailed(std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    bool initialized() const { return ready_; }

private:
    bool send(const ble::Bytes& frame);
    bool sendGroup(const std::vector<ble::Bytes>& frames);
    bool named(const char* name) const;
    const ifit_bowflex::Model* model_;
    bool manualProfile_ = false;
    std::vector<ble::Bytes> init_, poll_;
    std::size_t step_ = 0, pollStep_ = 0;
    std::uint32_t sentMs_ = 0, connectedMs_ = 0, cadenceMs_ = 0;
    std::uint16_t crank_ = 0;
    bool connected_ = false, ready_ = false, crankKnown_ = false;
    bool waitingMarker_ = false;
    ble::ConnectPlan customPlan_;
    ble::Uuid writeService_, writeCharacteristic_;
    std::uint32_t subscribedMask_ = 0;
};

}  // namespace protocols
}  // namespace smartfitness
