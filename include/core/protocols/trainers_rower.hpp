#pragma once

// BLE ports derived from qdomyos-zwift (Roberto Viola and contributors),
// GPL-3.0, revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/tacxneo2/tacxneo2.cpp, stagesbike/stagesbike.cpp,
// wahookickrsnapbike/wahookickrsnapbike.cpp, concept2skierg/concept2skierg.cpp,
// ftmsrower/ftmsrower.cpp and bluetooth.cpp (all under src/devices).

#include "protocols/source_protocol.hpp"
#include "protocols/sensor_codec.hpp"

namespace smartfitness {
namespace protocols {

namespace trainerrower {
extern const char* const kTacxService;
extern const char* const kTacxNotify;
extern const char* const kTacxWrite;
extern const char* const kEliteService;
extern const char* const kEliteWrite;
extern const char* const kEliteConfig;
extern const char* const kWahooControl;
extern const char* const kConcept2Service;
ble::Bytes tacxPower(float watts);
ble::Bytes tacxGrade(float percent);
ble::Bytes elitePower(float watts);
ble::Bytes eliteBrake(float percent);
ble::Bytes eliteSimulation(float percent);
ble::Bytes wahooPower(float watts);
ble::Bytes wahooBrake(float percent);
ble::Bytes wahooGrade(float percent);
}

// Shared standard CPS/CSC/HR telemetry, without fabricated power-to-speed.
class TrainerSensorSource : public SourceProtocol {
public:
    void reset() override;
    void tick(std::uint32_t nowMs) override;
protected:
    bool sensorNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs);
    static ble::ConnectPlan sensorPlan();
    sensors::RevolutionRate crank_{1024, 16};
    sensors::RevolutionRate cscWheel_{1024, 32};
    sensors::RevolutionRate powerWheel_{2048, 32};
    bool cscCadence_ = false;
    bool cscSpeed_ = false;
};

class TacxFecSource final : public TrainerSensorSource {
public:
    const char* id() const override { return "tacx-fec"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t mask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    void onWriteFailed(std::uint32_t nowMs) override;
private:
    bool connected_ = false;
};

class EliteTrainerSource final : public TrainerSensorSource {
public:
    explicit EliteTrainerSource(bool brakeOnly = false, bool passive = false) : brakeOnly_(brakeOnly), passive_(passive) {}
    const char* id() const override { return passive_ ? "elite-misuro" : brakeOnly_ ? "elite-dr" : "elite-legacy"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t mask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    void onWriteFailed(std::uint32_t nowMs) override;
private:
    bool brakeOnly_;
    bool passive_;
    bool connected_ = false;
    bool ready_ = false;
    std::uint32_t initMs_ = 0;
};

class WahooKickrSource final : public TrainerSensorSource {
public:
    const char* id() const override { return "wahoo-legacy"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t mask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
    void onWriteFailed(std::uint32_t nowMs) override;
private:
    unsigned stage_ = 0;
    std::uint32_t stageMs_ = 0;
    bool acknowledged_ = false;
    bool simulation_ = false;
};

class Concept2Pm5Source final : public SourceProtocol {
public:
    const char* id() const override { return "concept2-pm5"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t mask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;
private:
    bool active_ = true;
    bool multiplex_ = false;
};

bool matchesTacxFec(const ble::Advertisement& advertisement);
bool matchesEliteLegacy(const ble::Advertisement& advertisement);
bool matchesEliteDr(const ble::Advertisement& advertisement);
bool matchesEliteMisuro(const ble::Advertisement& advertisement);
bool matchesWahooKickr(const ble::Advertisement& advertisement);
bool matchesConcept2Pm5(const ble::Advertisement& advertisement);
std::unique_ptr<SourceProtocol> makeTacxFec();
std::unique_ptr<SourceProtocol> makeEliteLegacy();
std::unique_ptr<SourceProtocol> makeEliteDr();
std::unique_ptr<SourceProtocol> makeEliteMisuro();
std::unique_ptr<SourceProtocol> makeWahooKickr();
std::unique_ptr<SourceProtocol> makeConcept2Pm5();

}  // namespace protocols
}  // namespace smartfitness
