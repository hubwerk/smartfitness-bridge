// Toorx / TRX App Gate family protocol.
//
// GPL-3.0 port of qdomyos-zwift, Copyright (C) Roberto Viola and
// contributors. Pinned revision ea70d7ff64bc8bf508fc6401fd056211e05311b3:
// src/devices/trxappgateusbtreadmill/trxappgateusbtreadmill.{cpp,h},
// src/devices/trxappgateusbbike/trxappgateusbbike.{cpp,h},
// src/devices/trxappgateusbelliptical/trxappgateusbelliptical.{cpp,h},
// src/devices/trxappgateusbrower/trxappgateusbrower.{cpp,h},
// src/devices/bluetooth.cpp (advertisement rules), src/qzsettings.h
// (setting defaults). This port is part of smartfitness-bridge and
// distributed under the same license.
//
// Despite the "usb" in the upstream class names these are BLE devices. The
// RFCOMM-based toorxtreadmill, iconsolebike, iconceptbike and
// iconceptelliptical classes are not ported. Upstream QSettings toggles that
// change the wire protocol (toorx_bike, iconsole_elliptical, iconsole_rower,
// toorx_3_0, dkn_endurun_treadmill, ...) become manual-only descriptors.
// Upstream automatically starts the treadmill belt when the console reports
// "ready"; this port never starts, stops or changes the belt speed.

#include "protocols/toorx.hpp"

#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace smartfitness {
namespace protocols {
namespace toorx {

const ble::Uuid kFff0Service = ble::Uuid::from16(0xfff0);
const ble::Uuid kFff2Write = ble::Uuid::from16(0xfff2);
const ble::Uuid kFff1Notify = ble::Uuid::from16(0xfff1);
const ble::Uuid kIsscService = ble::Uuid::fromString("49535343-fe7d-4ae5-8fa9-9fafd205e455");
const ble::Uuid kIsscWrite = ble::Uuid::fromString("49535343-8841-43f4-a8d4-ecbe34729bb3");
const ble::Uuid kIsscNotify = ble::Uuid::fromString("49535343-1e4d-4bd9-ba61-23c647249616");
const ble::Uuid kIsscNotify2 = ble::Uuid::fromString("49535343-4c8a-39b3-2f49-511cff073b7e");
const ble::Uuid kIsscNotify3 = ble::Uuid::fromString("49535343-aca3-481c-91ec-d85e28a60318");

namespace {

using K = Kind;
using P = Parse;
using S = Service;

// ---- Treadmill init frames (trxappgateusbtreadmill::btinit) ----
constexpr Frame kTmTrx[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {6, {0xf0, 0xa5, 0x01, 0xd3, 0x04, 0x6d}},
    {5, {0xf0, 0xa0, 0x01, 0xd3, 0x64}},
    {5, {0xf0, 0xa1, 0x01, 0xd3, 0x65}},
    {12, {0xf0, 0xa3, 0x01, 0xd3, 0x01, 0x15, 0x01, 0x02, 0x51, 0x01, 0x51, 0x23}},
    {16, {0xf0, 0xa4, 0x01, 0xd3, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x73}},
    {6, {0xf0, 0xaf, 0x01, 0xd3, 0x02, 0x75}}};
constexpr std::uint8_t kTmTrxOrder[] = {0, 1, 2, 3, 2, 4, 5, 6};
// i-Running and JTX Sprint add "a1, a0, a0" after the first five writes.
constexpr std::uint8_t kTmIrunOrder[] = {0, 1, 2, 3, 2, 3, 2, 2, 4, 5, 6};

// Toorx 3.0 treadmill and the bike default branch share these frames.
constexpr Frame kTrx30[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {6, {0xf0, 0xa5, 0x23, 0xd3, 0x04, 0x8f}},
    {5, {0xf0, 0xa0, 0x23, 0xd3, 0x86}},
    {5, {0xf0, 0xa1, 0x23, 0xd3, 0x87}},
    {12, {0xf0, 0xa3, 0x23, 0xd3, 0x01, 0x15, 0x01, 0x02, 0x51, 0x01, 0x51, 0x45}},
    {16, {0xf0, 0xa4, 0x23, 0xd3, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x95}},
    {6, {0xf0, 0xaf, 0x23, 0xd3, 0x02, 0x97}}};
constexpr std::uint8_t kSeq7[] = {0, 1, 2, 3, 4, 5, 6};

constexpr Frame kTmDkn[] = {
    {5, {0xf0, 0xa0, 0x02, 0x02, 0x94}},
    {5, {0xf0, 0xa0, 0x01, 0xd3, 0x64}},
    {6, {0xf0, 0xa5, 0x01, 0xd3, 0x04, 0x6d}},
    {5, {0xf0, 0xa1, 0x01, 0xd3, 0x65}},
    {12, {0xf0, 0xa3, 0x01, 0xd3, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x47, 0xb7}},
    {8, {0xf0, 0xac, 0x01, 0xd3, 0x01, 0x64, 0x64, 0x39}},
    {16, {0xf0, 0xa4, 0x01, 0xd3, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x0b, 0x01, 0x01, 0x7d}},
    {6, {0xf0, 0xaf, 0x01, 0xd3, 0x02, 0x75}}};
constexpr Frame kTmAdidas[] = {
    {5, {0xf0, 0xa0, 0x02, 0x02, 0x94}},
    {5, {0xf0, 0xa0, 0x5b, 0xd3, 0xbe}},
    {6, {0xf0, 0xa5, 0x5b, 0xd3, 0x04, 0xc7}},
    {5, {0xf0, 0xa1, 0x5b, 0xd3, 0xbf}},
    {12, {0xf0, 0xa3, 0x5b, 0xd3, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x47, 0x11}},
    {8, {0xf0, 0xac, 0x5b, 0xd3, 0x01, 0x64, 0x64, 0x93}},
    {16, {0xf0, 0xa4, 0x5b, 0xd3, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x0b, 0x01, 0x01, 0xd7}},
    {6, {0xf0, 0xaf, 0x5b, 0xd3, 0x02, 0xcf}}};
constexpr std::uint8_t kTmDknOrder[] = {0, 1, 2, 2, 2, 3, 4, 5, 5, 5, 6, 7, 2};

constexpr Frame kTmReebok[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x32, 0xd3, 0x95}},
    {5, {0xf0, 0xa1, 0x32, 0xd3, 0x96}},
    {6, {0xf0, 0xa5, 0x32, 0xd3, 0x04, 0x9e}},
    {12, {0xf0, 0xa3, 0x32, 0xd3, 0x01, 0x15, 0x01, 0x02, 0x51, 0x01, 0x51, 0x54}},
    {16, {0xf0, 0xa4, 0x32, 0xd3, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xa4}},
    {6, {0xf0, 0xaf, 0x32, 0xd3, 0x02, 0xa6}}};
constexpr std::uint8_t kTmReebokOrder[] = {0, 0, 1, 2, 3, 1, 4, 5, 6};

constexpr Frame kTmDknRun[] = {
    {5, {0xf0, 0xa0, 0x04, 0x01, 0x95}},
    {6, {0xf0, 0xa5, 0x04, 0x01, 0x04, 0x9e}},
    {5, {0xf0, 0xa1, 0x04, 0x01, 0x96}},
    {6, {0xf0, 0xa3, 0x04, 0x01, 0x04, 0x9c}},
    {8, {0xf0, 0xac, 0x04, 0x01, 0x01, 0x64, 0x64, 0x6a}},
    {16, {0xf0, 0xa4, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x01, 0x01, 0xa6}},
    {16, {0xf0, 0xa4, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xa4}},
    {16, {0xf0, 0xa4, 0x04, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x06, 0x01, 0x01, 0xa9}},
    {5, {0xf0, 0xa2, 0x04, 0x01, 0x97}},
    {6, {0xf0, 0xa5, 0x04, 0x01, 0x02, 0x9c}}};
constexpr std::uint8_t kTmDknRunOrder[] = {0, 1, 1, 1, 2, 3, 4, 4, 4, 5, 6, 7, 8, 9};

constexpr Frame kTmFlow[] = {
    {5, {0xf0, 0xa0, 0x02, 0x02, 0x94}},
    {5, {0xf0, 0xa0, 0x2e, 0xd3, 0x91}},
    {6, {0xf0, 0xa5, 0x2e, 0xd3, 0x04, 0x9a}},
    {5, {0xf0, 0xa1, 0x2e, 0xd3, 0x92}},
    {8, {0xf0, 0xac, 0x2e, 0xd3, 0x01, 0x64, 0x64, 0x66}}};
constexpr std::uint8_t kSeq5[] = {0, 1, 2, 3, 4};

// ---- Bike init frames (trxappgateusbbike::btinit) ----
constexpr Frame kBkIrun[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa1, 0x01, 0x01, 0x93}},
    {6, {0xf0, 0xa3, 0x01, 0x01, 0x01, 0x96}},
    {6, {0xf0, 0xa5, 0x01, 0x01, 0x02, 0x99}},
    {6, {0xf0, 0xa6, 0x01, 0x01, 0x06, 0x9e}}};
constexpr Frame kBkHop[] = {
    {5, {0xf0, 0xa0, 0x01, 0x00, 0x91}},
    {5, {0xf0, 0xa0, 0x3f, 0x01, 0xd0}},
    {5, {0xf0, 0xa1, 0x3f, 0x01, 0xd1}},
    {6, {0xf0, 0xa3, 0x3f, 0x01, 0x01, 0xd4}},
    {15, {0xf0, 0xa4, 0x3f, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xde}},
    {6, {0xf0, 0xa5, 0x3f, 0x01, 0x02, 0xd7}}};
constexpr std::uint8_t kBkHopOrder[] = {0, 1, 2, 2, 1, 3, 4, 5};
// Toorx SRX 500 bike and Toorx elliptical share these frames.
constexpr Frame kTrx23[] = {
    {5, {0xf0, 0xa0, 0x01, 0x00, 0x91}},
    {5, {0xf0, 0xa0, 0x23, 0x01, 0xb4}},
    {5, {0xf0, 0xa1, 0x23, 0x01, 0xb5}},
    {6, {0xf0, 0xa3, 0x23, 0x01, 0x01, 0xb8}},
    {15, {0xf0, 0xa4, 0x23, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xc2}},
    {6, {0xf0, 0xa5, 0x23, 0x01, 0x02, 0xbb}}};
constexpr std::uint8_t kBkSrxOrder[] = {0, 1, 2, 1, 2, 1, 2, 3, 4, 5};
constexpr std::uint8_t kElToorxOrder[] = {0, 1, 2, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 3, 4, 5};
constexpr Frame kBkVfit[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x1e, 0x01, 0xaf}},
    {5, {0xf0, 0xa1, 0x1e, 0x01, 0xb0}},
    {6, {0xf0, 0xa3, 0x1e, 0x01, 0x01, 0xb3}},
    {15, {0xf0, 0xa4, 0x1e, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xbd}},
    {6, {0xf0, 0xa5, 0x1e, 0x01, 0x02, 0xb6}}};
constexpr std::uint8_t kBkVfitOrder[] = {0, 1, 2, 1, 2, 1, 3, 4, 5};
constexpr Frame kBkHertz[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x02, 0x01, 0x93}},
    {6, {0xf0, 0xa3, 0x01, 0x01, 0x01, 0x96}},
    {15, {0xf0, 0xa4, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xa0}},
    {6, {0xf0, 0xa5, 0x01, 0x01, 0x02, 0x99}}};
constexpr std::uint8_t kBkHertzOrder[] = {0, 0, 1, 0, 1, 0, 1, 0, 2, 3, 3, 4};
constexpr Frame kBkDkn[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x02, 0x01, 0x93}},
    {6, {0xf0, 0xa3, 0x02, 0x01, 0x01, 0x97}},
    {6, {0xf0, 0xa5, 0x02, 0x01, 0x02, 0x9a}},
    {5, {0x40, 0x00, 0x9a, 0x46, 0x20}},
    {6, {0xf0, 0xa6, 0x02, 0x01, 0x02, 0x9b}},
    {5, {0xf0, 0xa2, 0x02, 0x01, 0x95}},
    {6, {0xf0, 0xa6, 0x02, 0x01, 0x03, 0x9c}}};
constexpr std::uint8_t kBkDknOrder[] = {0, 0, 1, 2, 3, 3, 4, 6, 5, 7, 6, 5};
constexpr Frame kBkChangyow[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x23, 0x01, 0xb4}},
    {5, {0xf0, 0xa1, 0x23, 0x01, 0xb5}},
    {6, {0xf0, 0xa3, 0x23, 0x01, 0x01, 0xb8}},
    {6, {0xf0, 0xa5, 0x23, 0x01, 0x02, 0xbb}},
    {6, {0xf0, 0xa6, 0x23, 0x01, 0x06, 0xc0}}};
constexpr std::uint8_t kSeq6[] = {0, 1, 2, 3, 4, 5};
constexpr Frame kBkJll[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x03, 0xc9, 0x5c}},
    {5, {0xf0, 0xa1, 0x00, 0xc8, 0x59}},
    {5, {0xf0, 0xa0, 0x01, 0xc9, 0x5a}},
    {5, {0xf0, 0xa1, 0x05, 0xc8, 0x5e}},
    {5, {0xf0, 0xa2, 0x22, 0xc8, 0x7c}},
    {5, {0xf0, 0xa0, 0x39, 0xc9, 0x92}}};
constexpr std::uint8_t kBkJllOrder[] = {0, 1, 2, 3, 2, 3, 2, 3, 2, 3, 4, 5, 6};
constexpr Frame kBkAsviva[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x01, 0xc9, 0x5a}},
    {5, {0xf0, 0xa1, 0x00, 0xc8, 0x59}},
    {5, {0xf0, 0xa2, 0x00, 0xc8, 0x5a}}};
constexpr std::uint8_t kBkAsvivaOrder[] = {0, 1, 2, 1, 1, 2, 1, 1, 3, 1};
constexpr Frame kBkReebok[] = {
    {5, {0xf0, 0xa0, 0x32, 0x01, 0xc3}},
    {6, {0xf0, 0xa3, 0x32, 0x01, 0x01, 0xc7}},
    {15, {0xf0, 0xa4, 0x32, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xd1}},
    {6, {0xf0, 0xa5, 0x32, 0x01, 0x02, 0xca}}};
constexpr std::uint8_t kBkReebokOrder[] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3};
constexpr Frame kBkAdidas[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x5b, 0x01, 0xec}},
    {5, {0xf0, 0xa1, 0x5b, 0x01, 0xed}},
    {6, {0xf0, 0xa3, 0x5b, 0x01, 0x01, 0xf0}},
    {6, {0xf0, 0xa5, 0x5b, 0x01, 0x02, 0xf3}}};
constexpr Frame kBkTunturi[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x03, 0x01, 0x94}},
    {5, {0xf0, 0xa1, 0x03, 0x01, 0x95}},
    {6, {0xf0, 0xa3, 0x03, 0x01, 0x01, 0x98}},
    {6, {0xf0, 0xa5, 0x03, 0x01, 0x02, 0x9b}},
    {6, {0xf0, 0xa6, 0x03, 0x01, 0x06, 0xa0}},
    {6, {0xf0, 0xa6, 0x03, 0x01, 0x09, 0xa3}},
    {5, {0xf0, 0xa2, 0x03, 0x01, 0x96}}};
constexpr std::uint8_t kBkTunturiOrder[] = {0, 1, 2, 3, 4, 7, 7, 7, 5, 7, 6};
constexpr Frame kBkFal[] = {
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa0, 0x1b, 0x01, 0xac}},
    {5, {0xf0, 0xa1, 0x1b, 0x01, 0xad}},
    {6, {0xf0, 0xa3, 0x1b, 0x01, 0x01, 0xb0}},
    {6, {0xf0, 0xa5, 0x1b, 0x01, 0x02, 0xb3}}};
constexpr Frame kBkFytter[] = {
    {5, {0xf0, 0xa0, 0x00, 0x00, 0x90}},
    {5, {0xf0, 0xa0, 0x00, 0xc8, 0x58}}};
constexpr std::uint8_t kSeq2[] = {0, 1};
constexpr Frame kBkCasall[] = {
    {5, {0xf0, 0xa0, 0x00, 0x00, 0x90}},
    {5, {0xf0, 0xa0, 0x3b, 0x01, 0xcc}},
    {5, {0xf0, 0xa1, 0x3b, 0x01, 0xcd}},
    {6, {0xf0, 0xa5, 0x3b, 0x01, 0x03, 0xd4}},
    {5, {0xf0, 0xa2, 0x3b, 0x01, 0xce}},
    {6, {0xf0, 0xa3, 0x3b, 0x01, 0x01, 0xd0}},
    {15, {0xf0, 0xa4, 0x3b, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xda}},
    {6, {0xf0, 0xa5, 0x3b, 0x01, 0x02, 0xd3}}};
constexpr std::uint8_t kBkCasallOrder[] = {0, 1, 2, 3, 3, 4, 4, 4, 4, 5, 6, 7};
constexpr Frame kBkTaurua[] = {
    {5, {0xf0, 0xa0, 0x01, 0x00, 0x91}},
    {5, {0xf0, 0xa0, 0x01, 0x31, 0xc2}},
    {5, {0xf0, 0xa1, 0x01, 0x31, 0xc3}},
    {6, {0xf0, 0xa3, 0x01, 0x31, 0x01, 0xc6}},
    {15, {0xf0, 0xa4, 0x01, 0x31, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xd0}},
    {6, {0xf0, 0xa5, 0x01, 0x31, 0x02, 0xc9}}};
constexpr std::uint8_t kBkTauruaOrder[] = {0, 1, 2, 1, 2, 3, 4, 5};

// ---- Elliptical (trxappgateusbelliptical::btinit) ----
constexpr Frame kElGeneric[] = {
    {5, {0xf0, 0xa0, 0x01, 0x00, 0x91}},
    {5, {0xf0, 0xa0, 0x35, 0x01, 0xc6}},
    {5, {0xf0, 0xa1, 0x35, 0x01, 0xc7}},
    {6, {0xf0, 0xa3, 0x35, 0x01, 0x01, 0xca}},
    {15, {0xf0, 0xa4, 0x35, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xd4}},
    {6, {0xf0, 0xa5, 0x35, 0x01, 0x02, 0xcd}}};
constexpr Frame kElDct[] = {
    {5, {0xf0, 0xa0, 0x01, 0x00, 0x91}},
    {5, {0xf0, 0xa0, 0x01, 0x01, 0x92}},
    {5, {0xf0, 0xa1, 0x01, 0x01, 0x93}},
    {6, {0xf0, 0xa3, 0x01, 0x01, 0x01, 0x96}},
    {15, {0xf0, 0xa4, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xa0}},
    {6, {0xf0, 0xa5, 0x01, 0x01, 0x02, 0x99}}};
constexpr std::uint8_t kElOrder[] = {0, 1, 2, 1, 2, 1, 2, 1, 2, 1, 3, 4, 5};

// ---- Rower (trxappgateusbrower::btinit) ----
constexpr Frame kRwStandard[] = {
    {5, {0xf0, 0xa0, 0x01, 0x00, 0x91}},
    {5, {0xf0, 0xa0, 0x01, 0xe7, 0x78}},
    {5, {0xf0, 0xa1, 0x01, 0xe7, 0x79}},
    {6, {0xf0, 0xa3, 0x01, 0xe7, 0x01, 0x7c}},
    // Upstream checksum is wrong (sum is 0x86); sent verbatim.
    {15, {0xf0, 0xa4, 0x01, 0xe7, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x83}},
    {6, {0xf0, 0xa5, 0x01, 0xe7, 0x02, 0x7f}}};
constexpr std::uint8_t kRwOrder[] = {0, 0, 1, 2, 1, 2, 1, 2, 1, 2, 1, 3, 4, 5};
constexpr Frame kRwMearch[] = {
    {6, {0xf0, 0xa5, 0x44, 0x01, 0x04, 0xde}},
    {5, {0xf0, 0xa0, 0x44, 0x01, 0xd5}},
    {5, {0xf0, 0xa0, 0x01, 0xe8, 0x79}},
    {6, {0xf0, 0xa5, 0x01, 0xe8, 0x02, 0x80}}};
constexpr std::uint8_t kSeq4[] = {0, 1, 2, 3};

#define TRX_SEQ(frames, order) frames, order, static_cast<std::uint8_t>(sizeof(order))
#define TRX_POLL(a, b, c, d, e) Frame{5, {a, b, c, d, e}}

enum Index : std::uint8_t {
    vTm, vTmIrun, vTmReebok, vTmAdidas, vTmDknRun, vTm30, vTmJtx, vTmDkn, vTmFlow, vTmFr30,
    vBkTc, vBkTun, vBkFhw, vBkVfit, vBkIrun, vBkAdc1, vBkChy, vBkIcon, vBkCasall, vBkReebok, vBkDkn, vBkFal,
    vBkHertz, vBkEnerfit, vBkHammer, vBkJll, vBkFytter, vBkAsviva, vBkHop, vBkSrx500, vBkTaurua,
    vEl, vElToorx, vElDct, vRw, vRwMrk,
    kDescriptorCount,
    vElJtx = kDescriptorCount,  // internal: generic elliptical without service 0xFFF0
    kVariantCount
};

// clang-format off
const Variant kVariants[kVariantCount] = {
    // Treadmills: incline control frame f0 ac <A> <B> ...
    {"trx-tm", "Toorx / V-RUN / K80 treadmill", K::Treadmill, P::Treadmill, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kTmTrx, kTmTrxOrder), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0xd3, 0x66), 200, false, 0x01, 0xd3, 0, 0, 0, false},
    {"trx-tm-irun", "i-Running / iConsole+ treadmill", K::Treadmill, P::Treadmill, S::Issc, S::Fff0, kSame, 0, kSame,
     TRX_SEQ(kTmTrx, kTmIrunOrder), 400, TRX_POLL(0xf0, 0xa2, 0x01, 0xd3, 0x66), 200, false, 0x01, 0xd3, 0, 0, 0, false},
    {"trx-tm-reebok", "Reebok treadmill (TRX)", K::Treadmill, P::Treadmill, S::Issc, S::Fff0, kSame, 0, kSame,
     TRX_SEQ(kTmReebok, kTmReebokOrder), 400, TRX_POLL(0xf0, 0xa2, 0x32, 0xd3, 0x97), 200, false, 0x01, 0xd3, 0, 0, 0, false},
    {"trx-tm-adidas", "Adidas treadmill (TRX)", K::Treadmill, P::Treadmill, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kTmAdidas, kTmDknOrder), 400, TRX_POLL(0xf0, 0xa2, 0x5b, 0xd3, 0xc0), 200, false, 0x5b, 0xd3, 0, 0, 0, true},
    {"trx-tm-dknrun", "DKN Run treadmill", K::Treadmill, P::TreadmillDkn, S::Issc, S::None, kSame, 2, kSame,
     TRX_SEQ(kTmDknRun, kTmDknRunOrder), 400, TRX_POLL(0xf0, 0xa2, 0x04, 0x01, 0x97), 200, false, 0x01, 0xd3, 0, 0, 0, true},
    {"trx-tm-30", "Toorx treadmill (Toorx 3.0 protocol)", K::Treadmill, P::Treadmill, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kTrx30, kSeq7), 0, TRX_POLL(0xf0, 0xa2, 0x23, 0xd3, 0x88), 200, false, 0x01, 0xd3, 0, 0, 0, false},
    {"trx-tm-jtx", "JTX Fitness Sprint treadmill", K::Treadmill, P::Treadmill, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kTmTrx, kTmIrunOrder), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0xd3, 0x66), 200, false, 0x01, 0xd3, 0, 0, 0, false},
    {"trx-tm-dkn", "DKN Endurun treadmill", K::Treadmill, P::Treadmill, S::Fff0, S::Issc, kSame, 0, kSame,
     TRX_SEQ(kTmDkn, kTmDknOrder), 400, TRX_POLL(0xf0, 0xa2, 0x01, 0xd3, 0x66), 200, false, 0x01, 0xd3, 0, 0, 0, true},
    {"trx-tm-flow", "Flow Fitness DTM2000i treadmill", K::Treadmill, P::TreadmillFlow, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kTmFlow, kSeq5), 0, TRX_POLL(0xf0, 0xa2, 0x2e, 0xd3, 0x93), 200, false, 0x2e, 0xd3, 0, 0, 0, false},
    {"trx-tm-fr30", "Reebok FR30 treadmill", K::Treadmill, P::Treadmill, S::Issc, S::Fff0, kSame, 0, kSame,
     TRX_SEQ(kTmReebok, kTmReebokOrder), 400, TRX_POLL(0xf0, 0xa2, 0x32, 0xd3, 0x97), 200, false, 0x32, 0xd3, 0, 0, 0, false},

    // Bikes: resistance frame f0 a6 <A> <B> <level+1> <sum>, levels 1..32.
    {"trx-bike-tc", "TRX App Gate TC bike", K::Bike, P::Bike, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kTrx30, kSeq7), 0, TRX_POLL(0xf0, 0xa2, 0x23, 0xd3, 0x88), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-tun", "Tunturi bike (TRX)", K::Bike, P::BikeTunturi, S::Issc, S::Fff0, kSame, 0, kSame,
     TRX_SEQ(kBkTunturi, kBkTunturiOrder), 400, TRX_POLL(0xf0, 0xa2, 0x03, 0x01, 0x96), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-fhw", "FitHiWay bike", K::Bike, P::Bike, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkHertz, kBkHertzOrder), 0, TRX_POLL(0xf0, 0xa2, 0x23, 0xd3, 0x88), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-vfit", "VirtuFit bike (TRX)", K::Bike, P::Bike, S::Fff0, S::Issc, kSame, 0, kSame,
     TRX_SEQ(kBkVfit, kBkVfitOrder), 0, TRX_POLL(0xf0, 0xa2, 0x1e, 0x01, 0xb1), 200, false, 0x1e, 0x01, 0, 1, 32, false},
    {"trx-bike-irun", "Toorx / iBiking+ bike (i-Running)", K::Bike, P::Bike, S::Issc, S::Fff0, kSame, 1, vBkIcon,
     TRX_SEQ(kBkIrun, kSeq5), 400, TRX_POLL(0xf0, 0xa2, 0x01, 0x01, 0x94), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-adc1", "Adidas C1 bike", K::Bike, P::Bike, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkAdidas, kSeq5), 400, TRX_POLL(0xf0, 0xa2, 0x5b, 0x01, 0xee), 200, false, 0x5b, 0x01, 0, 1, 32, false},
    {"trx-bike-chy", "Changyow bike (TRX)", K::Bike, P::Bike, S::Issc, S::None, kSame, 1, vBkIcon,
     TRX_SEQ(kBkChangyow, kSeq6), 400, TRX_POLL(0xf0, 0xa2, 0x23, 0x01, 0xb6), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-icon", "iConsole+ bike (TRX)", K::Bike, P::Bike, S::Issc, S::Fff0, kSame, 0, kSame,
     TRX_SEQ(kBkIrun, kSeq5), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0x01, 0x94), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-casall", "Casall bike (I-CONSOIE+)", K::Bike, P::Bike, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkCasall, kBkCasallOrder), 400, TRX_POLL(0xf0, 0xa2, 0x3b, 0x01, 0xce), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-reebok", "Reebok bike (TRX)", K::Bike, P::Bike, S::Fff0, S::Issc, kSame, 0, kSame,
     TRX_SEQ(kBkReebok, kBkReebokOrder), 400, TRX_POLL(0xf0, 0xa2, 0x32, 0x01, 0xc5), 200, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-dkn", "DKN Motion bike", K::Bike, P::Bike, S::Fff0, S::Issc, kSame, 0, kSame,
     TRX_SEQ(kBkDkn, kBkDknOrder), 0, TRX_POLL(0xf0, 0xa2, 0x02, 0x01, 0x95), 200, false, 0x01, 0x01, 0x02, 1, 32, false},
    {"trx-bike-fal", "FAL-SPORTS bike", K::Bike, P::BikeFal, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkFal, kSeq5), 400, TRX_POLL(0xf0, 0xa2, 0x1b, 0x01, 0xae), 200, false, 0x1b, 0x01, 0, 1, 32, false},
    {"trx-bike-hertz", "Hertz XR-770 bike", K::Bike, P::Bike, S::Fff0, S::Issc, kSame, 0, kSame,
     TRX_SEQ(kBkHertz, kBkHertzOrder), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0x01, 0x94), 500, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-enerfit", "Enerfit SPX 9500 bike", K::Bike, P::Bike, S::Fff0, S::Issc, kSame, 0, kSame,
     TRX_SEQ(kBkChangyow, kSeq6), 400, TRX_POLL(0xf0, 0xa2, 0x23, 0x01, 0xb6), 500, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-hammer", "Hammer Speed Bike S (iBiking+)", K::Bike, P::BikeRaw19, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkChangyow, kSeq6), 400, TRX_POLL(0xf0, 0xa2, 0x23, 0x01, 0xb6), 500, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-jll", "JLL IC400 bike", K::Bike, P::BikeRaw19, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkJll, kBkJllOrder), 400, TRX_POLL(0xf0, 0xa2, 0x0b, 0xc8, 0x65), 500, true, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-fytter", "Fytter RI-08 bike", K::Bike, P::BikeFytter, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkFytter, kSeq2), 400, TRX_POLL(0xf0, 0xa2, 0x00, 0xc8, 0x5a), 500, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-asviva", "Asviva bike", K::Bike, P::BikeRaw19, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkAsviva, kBkAsvivaOrder), 400, TRX_POLL(0xf0, 0xa2, 0x00, 0xc8, 0x5a), 500, false, 0x01, 0x01, 0, 1, 32, false},
    {"trx-bike-hop", "Hop-Sport HS-090H bike", K::Bike, P::Bike, S::Fff0, S::Issc, kSame, 0, kSame,
     TRX_SEQ(kBkHop, kBkHopOrder), 0, TRX_POLL(0xf0, 0xa2, 0x3f, 0x01, 0xd2), 500, false, 0x3f, 0x01, 0, 1, 32, false},
    {"trx-bike-srx500", "Toorx SRX 500 bike", K::Bike, P::BikeSrx500, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kTrx23, kBkSrxOrder), 0, TRX_POLL(0xf0, 0xa2, 0x23, 0x01, 0xb6), 500, false, 0x23, 0x01, 0, 1, 32, false},
    {"trx-bike-taurua", "Taurua IC90 bike", K::Bike, P::Bike, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kBkTaurua, kBkTauruaOrder), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0x31, 0xc4), 500, false, 0x01, 0x01, 0, 1, 32, false},

    // Ellipticals: resistance frame f0 a6 <A> <B> <level+1> <sum>.
    {"trx-ell", "FAL-SPORTS elliptical (TRX)", K::Elliptical, P::Elliptical, S::Fff0, S::Issc, vElJtx, 0, kSame,
     TRX_SEQ(kElGeneric, kElOrder), 0, TRX_POLL(0xf0, 0xa2, 0x35, 0x01, 0xc8), 200, false, 0x35, 0x01, 0, 1, 15, true},
    {"trx-ell-toorx", "Toorx elliptical", K::Elliptical, P::EllipticalToorx, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kTrx23, kElToorxOrder), 0, TRX_POLL(0xf0, 0xa2, 0x23, 0x01, 0xb6), 500, false, 0x23, 0x01, 0, 1, 32, true},
    {"trx-ell-dct", "iConsole+ elliptical (DCT2000i / Taurus FX9.9)", K::Elliptical, P::Elliptical, S::Issc, S::Fff0, kSame, 0, kSame,
     TRX_SEQ(kElDct, kElOrder), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0x01, 0x94), 200, false, 0x01, 0x01, 0, 1, 15, true},

    // Rowers: resistance frame f0 a6 35 01 <level+1> <sum>, levels 1..15.
    {"trx-row", "iConsole+ rower", K::Rower, P::Rower, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kRwStandard, kRwOrder), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0xe7, 0x7a), 200, false, 0x35, 0x01, 0, 1, 15, true},
    {"trx-row-mrk", "Mearch NovaRow R50 rower", K::Rower, P::RowerMearch, S::Fff0, S::None, kSame, 0, kSame,
     TRX_SEQ(kRwMearch, kSeq4), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0xe8, 0x7b), 200, false, 0x35, 0x01, 0, 1, 15, true},

    // Internal: JTX Fitness behaviour of the generic elliptical on the ISSC service.
    {"trx-ell", "JTX Fitness elliptical", K::Elliptical, P::EllipticalJtx, S::Issc, S::None, kSame, 0, kSame,
     TRX_SEQ(kElDct, kElOrder), 0, TRX_POLL(0xf0, 0xa2, 0x01, 0x01, 0x94), 200, false, 0x01, 0x01, 0, 1, 15, true},
};
// clang-format on

#undef TRX_SEQ
#undef TRX_POLL

const ble::Uuid& serviceUuid(Service service) { return service == S::Issc ? kIsscService : kFff0Service; }
const ble::Uuid& writeUuid(Service service) { return service == S::Issc ? kIsscWrite : kFff2Write; }
const ble::Uuid& notifyUuid(Service service) { return service == S::Issc ? kIsscNotify : kFff1Notify; }

int byteAt(const std::uint8_t* data, std::size_t index) { return static_cast<int>(data[index]); }
int nonNegative(int value) { return value < 0 ? 0 : value; }
// Upstream "value + 1" two-digit fields: (hi - 1) * 100 + (lo - 1).
int pair100(const std::uint8_t* d, std::size_t hi, std::size_t lo) {
    return nonNegative((byteAt(d, hi) - 1) * 100 + (byteAt(d, lo) - 1));
}
// Upstream estimate used when the console has no speed field.
constexpr float kSpeedFromCadence = 0.37407407407407407F;
// Default QZSettings::cadence_sensor_speed_ratio used by the JTX elliptical.
constexpr float kJtxSpeedRatio = 0.33F;

void set(MetricSample& sample, float value) { sample = {value, true}; }

bool carriesHeart(Parse parse) {
    switch (parse) {
        case P::Bike: case P::BikeSrx500: case P::BikeTunturi: case P::BikeFal: case P::BikeRaw19:
        case P::BikeFytter: case P::RowerMearch:
            return true;
        default:
            return false;
    }
}

std::string upper(const std::string& value) {
    std::string out(value);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
bool startsWith(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }
bool ftms(const ble::Advertisement& a) { return a.hasService(0x1826); }

// ^ADIDAS\d{4,}$ (case insensitive)
bool adidasC1Name(const std::string& name) {
    const std::string u = upper(name);
    if (!startsWith(u, "ADIDAS") || u.size() < 10) return false;
    for (std::size_t i = 6; i < u.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(u[i]))) return false;
    return true;
}
bool tcName(const std::string& name) {
    if (name.size() != 5 || !startsWith(upper(name), "TC")) return false;
    for (std::size_t i = 2; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    return true;
}

// trxappgateusbbike::deviceDiscovered type selection for names that reach
// the class with default settings (bluetooth.cpp, !toorx_bike).
int bikeVariant(const ble::Advertisement& a) {
    if (ftms(a)) return -1;
    const std::string u = upper(a.name);
    const bool tc = tcName(a.name);
    const bool ri009r = u.find("RI009R") != std::string::npos;
    const bool adidas = adidasC1Name(a.name);
    const bool reaches = tc || startsWith(u, "TUN ") || startsWith(u, "FITHIWAY") || startsWith(u, "FIT HI WAY") ||
                         startsWith(u, "PASYOU-") || startsWith(u, "VIRTUFIT") || startsWith(u, "IBIKING+") ||
                         ri009r || adidas;
    if (!reaches) return -1;
    if (adidas) return vBkAdc1;
    if (startsWith(u, "TUN ")) return vBkTun;
    if (startsWith(u, "VIRTUFIT")) return vBkVfit;
    if (startsWith(u, "FITHIWAY") || startsWith(u, "FIT HI WAY")) return vBkFhw;
    if (a.address.bytes[0] == 0xe8) return vBkChy;
    // PASYOU: upstream's length filter (21 and 19 bytes at once) drops every frame.
    if (startsWith(u, "PASYOU-")) return -1;
    if (tc) return vBkTc;
    return vBkIrun;  // IBIKING+, RI009R: upstream default type IRUNNING
}

template <int I>
bool matchBike(const ble::Advertisement& a) { return bikeVariant(a) == I; }

bool matchTreadmill(const ble::Advertisement& a) {
    const std::string u = upper(a.name);
    return !ftms(a) && (startsWith(a.name, "TOORX") || startsWith(a.name, "V-RUN") || startsWith(u, "K80_"));
}
bool matchTreadmillIrun(const ble::Advertisement& a) {
    const std::string u = upper(a.name);
    return !ftms(a) && (startsWith(u, "I-RUNNING") || startsWith(u, "I-CONSOLE+") || startsWith(u, "ICONSOLE+"));
}
bool matchTreadmillReebok(const ble::Advertisement& a) { return !ftms(a) && startsWith(upper(a.name), "REEBOK"); }
bool matchTreadmillAdidas(const ble::Advertisement& a) { return !ftms(a) && startsWith(upper(a.name), "ADIDAS "); }
// Names "DK..." of 11+ characters are sent to horizontreadmill (FTMS) first.
bool matchTreadmillDknRun(const ble::Advertisement& a) {
    return !ftms(a) && startsWith(upper(a.name), "DKN RUN") && a.name.size() < 11;
}
bool matchElliptical(const ble::Advertisement& a) { return !ftms(a) && startsWith(upper(a.name), "FAL-SPORTS"); }
bool manualOnly(const ble::Advertisement&) { return false; }

template <std::size_t I>
std::unique_ptr<SourceProtocol> make() { return std::unique_ptr<SourceProtocol>(new ToorxSource(I)); }

MachineProfile profileOf(Kind kind) {
    switch (kind) {
        case K::Treadmill: return MachineProfile::Treadmill;
        case K::Elliptical: return MachineProfile::CrossTrainer;
        case K::Rower: return MachineProfile::Rower;
        default: return MachineProfile::IndoorBike;
    }
}

using Matcher = bool (*)(const ble::Advertisement&);
constexpr Matcher kMatchers[kDescriptorCount] = {
    matchTreadmill, matchTreadmillIrun, matchTreadmillReebok, matchTreadmillAdidas, matchTreadmillDknRun,
    manualOnly, manualOnly, manualOnly, manualOnly, manualOnly,
    matchBike<vBkTc>, matchBike<vBkTun>, matchBike<vBkFhw>, matchBike<vBkVfit>, matchBike<vBkIrun>,
    matchBike<vBkAdc1>, matchBike<vBkChy>,
    manualOnly, manualOnly, manualOnly, manualOnly, manualOnly, manualOnly, manualOnly, manualOnly,
    manualOnly, manualOnly, manualOnly, manualOnly, manualOnly, manualOnly,
    matchElliptical, manualOnly, manualOnly,
    manualOnly, manualOnly};

template <std::size_t... I>
std::vector<ProtocolInfo> buildDescriptors(std::index_sequence<I...>) {
    return {ProtocolInfo{kVariants[I].id, kVariants[I].label,
                         kMatchers[I] == manualOnly ? 0 : kMatchPriority, kMatchers[I], make<I>,
                         kVariants[I].kind == K::Treadmill || kVariants[I].maxLevel > 0, false,
                         profileOf(kVariants[I].kind)}...};
}

const std::vector<ProtocolInfo>& descriptors() {
    static const std::vector<ProtocolInfo> values = buildDescriptors(std::make_index_sequence<kDescriptorCount>{});
    return values;
}

}  // namespace

std::size_t variantCount() { return kVariantCount; }
const Variant& variantAt(std::size_t index) { return kVariants[index < kVariantCount ? index : 0]; }

Bytes withSum(Bytes frame) {
    std::uint8_t sum = 0;
    for (auto byte : frame) sum = static_cast<std::uint8_t>(sum + byte);
    frame.push_back(sum);
    return frame;
}

Bytes frameBytes(const Frame& frame) { return Bytes(frame.bytes, frame.bytes + frame.length); }

Bytes resistanceFrame(std::uint8_t a, std::uint8_t b, int level) {
    return withSum({0xf0, 0xa6, a, b, static_cast<std::uint8_t>(level + 1)});
}

Bytes inclineFrame(std::uint8_t a, std::uint8_t b, float incline) {
    // Upstream: write[4] = (requestIncline + 1) converted to uint8_t (truncation).
    const auto value = static_cast<std::uint8_t>(static_cast<int>(incline + 1.0F));
    return withSum({0xf0, 0xac, a, b, value, 0x64, 0x64});
}

Bytes jllPollFrame(unsigned pollIndex) {
    const auto counter = static_cast<std::uint8_t>(0x0b + 0x0c * pollIndex);
    const auto crc = static_cast<std::uint8_t>(0x59 + 0x0c * (pollIndex + 1));
    return {0xf0, 0xa2, counter, 0xc8, crc};
}

bool decode(Parse parse, const std::uint8_t* d, std::size_t n, MetricSet& out) {
    out = {};
    if (!d) return false;
    const auto standardSpeed = [&] { return static_cast<float>(pair100(d, 6, 7)) / 10.0F; };
    const auto standardPower = [&] { return static_cast<float>(pair100(d, 16, 17)) / 10.0F; };
    const auto heart100 = [&] { return static_cast<float>(pair100(d, 14, 15)); };
    switch (parse) {
        case P::Treadmill:
        case P::TreadmillDkn:
        case P::TreadmillFlow: {
            const std::size_t expected = parse == P::TreadmillDkn ? 18 : 19;
            if (n != expected) return false;
            if (parse == P::TreadmillFlow &&
                (d[0] != 0xf0 || (d[1] != 0xb0 && d[1] != 0xb2) || d[2] != 0x2e || d[3] != 0xd3))
                return false;
            set(out.speed, static_cast<float>(pair100(d, 12, 13)) / 10.0F);
            set(out.incline, static_cast<float>(nonNegative(byteAt(d, 14) - 1)));
            return true;
        }
        case P::Bike:
        case P::BikeSrx500: {
            if (parse == P::Bike ? n != 21 : n < 19) return false;
            const float cadence = static_cast<float>(pair100(d, 8, 9));
            set(out.cadence, cadence);
            set(out.speed, standardSpeed());
            set(out.resistance, static_cast<float>(nonNegative(byteAt(d, 18) - 1)));
            set(out.power, cadence > 0 ? standardPower() : 0.0F);
            set(out.totalEnergy, static_cast<float>(pair100(d, 12, 13)));
            const float hr = heart100();
            out.heartRate = {hr, hr > 0};
            return true;
        }
        case P::BikeTunturi:
        case P::BikeFal: {
            if (parse == P::BikeTunturi ? (n != 20 && n != 21) : n < 19) return false;
            const float cadence = static_cast<float>(pair100(d, 8, 9));
            set(out.cadence, cadence);
            set(out.speed, cadence * kSpeedFromCadence);
            set(out.resistance, static_cast<float>(nonNegative(byteAt(d, 18) - 1)));
            set(out.power, cadence > 0 ? standardPower() : 0.0F);
            const float hr = heart100();
            out.heartRate = {hr, hr > 0};
            return true;
        }
        case P::BikeRaw19:
        case P::BikeFytter: {
            if (n != 19) return false;
            const float cadence = static_cast<float>(byteAt(d, 9) + byteAt(d, 8) * 100);
            set(out.cadence, cadence);
            set(out.speed, cadence * kSpeedFromCadence);
            if (parse == P::BikeFytter) {
                const float watt = d[6] ? static_cast<float>(nonNegative((byteAt(d, 6) - 1) * 100 + byteAt(d, 7))) : 0.0F;
                set(out.power, cadence > 0 ? watt : 0.0F);
                const float hr = heart100();
                out.heartRate = {hr, hr > 0};
            } else {
                const float hr = static_cast<float>(byteAt(d, 17) + byteAt(d, 16) * 83);
                out.heartRate = {hr, hr > 0};
            }
            return true;
        }
        case P::Elliptical:
        case P::EllipticalToorx:
        case P::EllipticalJtx: {
            if (n != 21) return false;
            set(out.resistance, static_cast<float>(nonNegative(byteAt(d, 18) - 1)));
            if (parse == P::EllipticalJtx) {
                const float cadence = static_cast<float>(byteAt(d, 5));
                set(out.cadence, cadence);
                set(out.speed, cadence * kJtxSpeedRatio);
                return true;
            }
            set(out.speed, standardSpeed());
            set(out.cadence, parse == P::EllipticalToorx ? static_cast<float>(pair100(d, 8, 9))
                                                         : static_cast<float>(byteAt(d, 9) + byteAt(d, 8) * 100));
            set(out.power, standardPower());
            return true;
        }
        case P::Rower: {
            if (n != 23) return false;
            set(out.resistance, static_cast<float>(nonNegative(byteAt(d, 18) - 1)));
            set(out.speed, standardSpeed());
            set(out.cadence, static_cast<float>(nonNegative(byteAt(d, 20) - 1)));
            set(out.power, static_cast<float>(pair100(d, 17, 18)) / 10.0F);
            return true;
        }
        case P::RowerMearch: {
            if (n != 23) return false;
            const auto pair99 = [&](std::size_t hi, std::size_t lo) {
                return static_cast<float>(nonNegative((byteAt(d, hi) - 1) * 99 + (byteAt(d, lo) - 1)));
            };
            set(out.elapsed, static_cast<float>(nonNegative((byteAt(d, 4) - 1) * 60 + (byteAt(d, 5) - 1))));
            set(out.strokeCount, pair99(6, 7));
            set(out.cadence, pair99(8, 9));
            set(out.distance, pair99(10, 11));
            set(out.totalEnergy, pair99(12, 13));
            // Upstream stores the heart rate in a uint8_t.
            const float hr = static_cast<float>(static_cast<std::uint8_t>(pair99(14, 15)));
            out.heartRate = {hr, hr > 0};
            set(out.power, pair99(16, 17));
            set(out.resistance, static_cast<float>(nonNegative(byteAt(d, 20) - 1)));
            return true;
        }
    }
    return false;
}

std::size_t descriptorCount() { return descriptors().size(); }
const ProtocolInfo& descriptorAt(std::size_t index) { return descriptors().at(index); }

}  // namespace toorx

using toorx::Variant;
using ble::Bytes;

ToorxSource::ToorxSource(std::size_t variantIndex)
    : base_(variantIndex < toorx::kDescriptorCount ? variantIndex : 0), active_(base_) {
    const Variant& v = toorx::variantAt(base_);
    std::size_t next = 1;
    if (v.fallback != toorx::Service::None) fallbackSub_ = next++;
    if (v.notify2) notify2Sub_ = next++;
}

const char* ToorxSource::id() const { return toorx::variantAt(base_).id; }
MachineProfile ToorxSource::profile() const { return toorx::profileOf(toorx::variantAt(base_).kind); }
const Variant& ToorxSource::activeVariant() const { return toorx::variantAt(active_); }

ble::ConnectPlan ToorxSource::plan() const {
    const Variant& v = toorx::variantAt(base_);
    ble::ConnectPlan plan;
    plan.subscribe(toorx::serviceUuid(v.primary), toorx::notifyUuid(v.primary));
    if (fallbackSub_ != 0xff) plan.subscribe(toorx::serviceUuid(v.fallback), toorx::notifyUuid(v.fallback));
    if (notify2Sub_ != 0xff)
        plan.subscribe(toorx::serviceUuid(v.primary), v.notify2 == 2 ? toorx::kIsscNotify3 : toorx::kIsscNotify2);
    return plan;
}

void ToorxSource::reset() {
    SourceProtocol::reset();
    active_ = base_;
    connected_ = ready_ = fallbackActive_ = notify2Active_ = false;
    step_ = 0;
    responded_ = false;
    sentMs_ = pollMs_ = 0;
    pollIndex_ = 0;
    buffer_.clear();
}

bool ToorxSource::onConnected(std::uint32_t mask, std::uint32_t nowMs) {
    const Variant& base = toorx::variantAt(base_);
    if (mask & 1U) {
        active_ = base_;
        fallbackActive_ = false;
    } else if (fallbackSub_ != 0xff && (mask & (1U << fallbackSub_))) {
        fallbackActive_ = true;
        active_ = base.fallbackIndex == toorx::kSame ? base_ : base.fallbackIndex;
    } else {
        setError("Toorx notify characteristic unavailable");
        return false;
    }
    if (!fallbackActive_ && notify2Sub_ != 0xff) {
        notify2Active_ = (mask & (1U << notify2Sub_)) != 0;
        // Upstream switches IRUNNING/CHANGYOW bikes to ICONSOLE without notify2.
        if (!notify2Active_ && base.noNotify2Index != toorx::kSame) active_ = base.noNotify2Index;
    }
    connected_ = true;
    step_ = 0;
    tick(nowMs);
    return true;
}

bool ToorxSource::send(const Bytes& payload) {
    const Variant& base = toorx::variantAt(base_);
    const toorx::Service service = fallbackActive_ ? base.fallback : base.primary;
    return queueWrite(toorx::serviceUuid(service), toorx::writeUuid(service), payload, activeVariant().withResponse);
}

void ToorxSource::tick(std::uint32_t nowMs) {
    if (!connected_) return;
    const Variant& v = activeVariant();
    // Upstream waits up to 300 ms for any notification after each write and
    // then sleeps initSleepMs before the next one.
    const bool waited = (responded_ && nowMs - sentMs_ >= v.initSleepMs) || nowMs - sentMs_ >= 300U + v.initSleepMs;
    if (!ready_) {
        if (step_ > 0 && !waited) return;
        if (step_ < v.steps) {
            if (pendingWrites()) return;
            send(toorx::frameBytes(v.frames[v.order[step_]]));
            ++step_;
            sentMs_ = nowMs;
            responded_ = false;
            return;
        }
        ready_ = true;
        pollMs_ = nowMs;
        return;
    }
    if (pendingWrites() || nowMs - pollMs_ < v.pollMs) return;
    pollMs_ = nowMs;
    send(v.pollCounter ? toorx::jllPollFrame(pollIndex_++) : toorx::frameBytes(v.poll));
}

void ToorxSource::publish(const MetricSet& metrics, std::uint32_t nowMs) {
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const auto metric = static_cast<Metric>(i);
        if (metrics[metric].valid) sample_[metric] = metrics[metric];
    }
    if (toorx::carriesHeart(activeVariant().parse)) sample_.heartRate = metrics.heartRate;
    touch(nowMs);
}

void ToorxSource::acceptTreadmill(const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    const Variant& v = activeVariant();
    const bool flow = v.parse == toorx::Parse::TreadmillFlow;
    const auto telemetry = [flow](std::uint8_t type) { return type == 0xb2 || (flow && type == 0xb0); };
    buffer_.insert(buffer_.end(), data, data + length);
    // Flow Fitness acknowledges commands with f0 b0 2e d3 a1.
    if (flow && buffer_ == Bytes{0xf0, 0xb0, 0x2e, 0xd3, 0xa1}) {
        buffer_.clear();
        return;
    }
    const bool fragmentable = buffer_.size() > 1 && buffer_[0] == 0xf0 && telemetry(buffer_[1]);
    if ((length < 18 && buffer_.size() > 2 && !fragmentable) || buffer_.size() > 19) {
        if (buffer_.size() == 3 && buffer_[1] == 0xf0 && telemetry(buffer_[2])) {
            buffer_ = Bytes{0xf0, buffer_[2]};
            return;
        }
        buffer_.clear();
        return;
    }
    const std::size_t expected = v.parse == toorx::Parse::TreadmillDkn ? 18 : 19;
    if (buffer_.size() != expected) return;
    MetricSet metrics;
    const bool ok = toorx::decode(v.parse, buffer_.data(), buffer_.size(), metrics);
    buffer_.clear();
    if (ok) publish(metrics, nowMs);
}

void ToorxSource::onNotify(std::size_t index, const std::uint8_t* data, std::size_t length, std::uint32_t nowMs) {
    if (!connected_ || !data) return;
    const bool selected = fallbackActive_ ? index == fallbackSub_ : (index == 0 || (notify2Active_ && index == notify2Sub_));
    if (!selected) return;
    responded_ = true;
    const Variant& v = activeVariant();
    if (v.kind == toorx::Kind::Treadmill) {
        acceptTreadmill(data, length, nowMs);
        return;
    }
    MetricSet metrics;
    if (toorx::decode(v.parse, data, length, metrics)) publish(metrics, nowMs);
}

bool ToorxSource::control(const ControlRequest& request, std::uint32_t nowMs) {
    if (!capabilities().supports(request.control) || !std::isfinite(request.value)) return false;
    const Variant& v = activeVariant();
    if (v.kind == toorx::Kind::Treadmill) {
        // Incline only while the treadmill is streaming fresh telemetry.
        if (request.value < 0.0F || request.value > toorx::kMaxIncline) return false;
        if (!hasData() || nowMs - lastDataMs() > 2000 || !sample_.incline.valid) return false;
        return send(toorx::inclineFrame(v.controlA, v.controlB, request.value));
    }
    const Variant& base = toorx::variantAt(base_);
    const std::uint8_t a = fallbackActive_ && base.fallbackControlA ? base.fallbackControlA : v.controlA;
    const int level = percentToLevel(request.value, v.minLevel, v.maxLevel);
    return send(toorx::resistanceFrame(a, v.controlB, level));
}

TrainerCapabilities ToorxSource::capabilities() const {
    TrainerCapabilities caps;
    caps.metrics = observedMetrics();
    const Variant& v = activeVariant();
    const bool usable = connected_ && ready_ && !failed();
    if (v.kind == toorx::Kind::Treadmill) caps.setControl(Control::Incline, usable);
    else caps.setControl(Control::Resistance, usable && v.maxLevel > 0);
    return caps;
}

}  // namespace protocols
}  // namespace smartfitness
