#pragma once

// Domyos / Decathlon bike protocol (Chang Yow init variant).
//
// Ported from qdomyos-zwift (https://github.com/cagnulein/qdomyos-zwift,
// src/devices/domyosbike/domyosbike.cpp), Copyright (C) Roberto Viola and
// contributors, licensed under GPL-3.0. This port is part of
// smartfitness-bridge and distributed under the same license.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "protocols/source_protocol.hpp"

namespace smartfitness {
namespace protocols {
namespace domyos {

using Bytes = ble::Bytes;

extern const ble::Uuid kService;  // 49535343-fe7d-4ae5-8fa9-9fafd205e455
extern const ble::Uuid kWrite;    // 49535343-8841-43f4-a8d4-ecbe34729bb3
extern const ble::Uuid kNotify;   // 49535343-1e4d-4bd9-ba61-23c647249616
constexpr int kMinResistance = 1;
constexpr int kMaxResistance = 15;
constexpr std::uint32_t kStepTimeoutMs = 300;
constexpr std::uint8_t kStepAttempts = 10;
constexpr std::uint32_t kPollIntervalMs = 300;

// Each init step is a group of writes; the step is acknowledged by any
// notification whose length is not 20 (upstream btinit_changyow()).
std::vector<std::vector<Bytes>> initSteps();
Bytes noOpFrame();
// 23-byte "force resistance" frame, sent as 20 + 3 bytes.
std::vector<Bytes> resistanceFrames(std::uint8_t level);
inline bool acknowledgesInit(std::size_t length) { return length != 20; }

// Reassembles the 26-byte status frame that some bikes split into a 20-byte
// head (f0 bc / f0 db / f0 dd) and a 6- or 7-byte tail.
class FrameAssembler {
public:
    // Returns true when `out` holds a complete 26-byte frame.
    bool feed(const std::uint8_t* data, std::size_t length, Bytes& out);
    // True while waiting for the tail of a split frame (polls are paused).
    bool incomplete() const { return incomplete_; }
    void reset();

private:
    Bytes last_;
    bool incomplete_ = false;
};

struct Status {
    float speed = 0.0F;     // km/h
    float cadence = 0.0F;   // rpm
    int resistance = 1;     // 1..15
    int heartRate = 0;      // bpm, 0 = no reading
    float distance = 0.0F;  // km
};
bool decodeStatus(const Bytes& frame, Status& out);
// Upstream default power model.
float power(int resistance, float cadence);

}  // namespace domyos

class DomyosSource final : public SourceProtocol {
public:
    const char* id() const override { return "domyos"; }
    ble::ConnectPlan plan() const override;
    bool onConnected(std::uint32_t subscribedMask, std::uint32_t nowMs) override;
    void onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) override;
    void tick(std::uint32_t nowMs) override;
    bool control(const ControlRequest& request, std::uint32_t nowMs) override;
    TrainerCapabilities capabilities() const override;
    void reset() override;

    bool initialized() const { return initDone_; }

private:
    void sendStep(std::uint32_t nowMs);

    std::vector<std::vector<domyos::Bytes>> steps_;
    std::size_t step_ = 0;
    std::uint8_t attempts_ = 0;
    std::uint32_t stepSentMs_ = 0;
    bool initializing_ = false;
    bool initDone_ = false;
    std::uint32_t lastPollMs_ = 0;
    domyos::FrameAssembler assembler_;
};

}  // namespace protocols
}  // namespace smartfitness
