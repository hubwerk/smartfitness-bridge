#include "ble_input_adapter.h"
#include "debug_log.h"
#include "fake_central.hpp"
#include "protocols/ftms_source.hpp"
#include "protocols/registry.hpp"

#include <cassert>
#include <string>

using namespace smartfitness;

// Keep this firmware-debug test independent of vendor adapters and radio hardware.
namespace smartfitness::protocols {
std::unique_ptr<SourceProtocol> createProtocol(const std::string&) { return std::make_unique<FtmsSource>(); }
const ProtocolInfo* detectProtocol(const ble::Advertisement&) { return nullptr; }
}  // namespace smartfitness::protocols

void testDebugBle() {
    const std::uint8_t bytes[] = {0, 0xab, 0xff};
    assert(std::string(debuglog::Hex(nullptr, 0).c_str()).empty());
    assert(std::string(debuglog::Hex(bytes, sizeof(bytes)).c_str()) == "00 ab ff");
    std::uint8_t longPayload[80]{};
    const std::string hex = debuglog::Hex(longPayload, sizeof(longPayload)).c_str();
    assert(hex.find("…(+16)") != std::string::npos);
    assert(std::string(debuglog::Hex(longPayload, 64, 100).c_str()).find("…(+36)") != std::string::npos);
    assert(std::string(debuglog::Value(std::nullopt).c_str()) == "-");
    assert(std::string(debuglog::Value(4.0F).c_str()) == "4.00");

    fake::now = 100;
    testing::FakeCentral central;
    BleInputAdapter input(central, [] { return fake::now; });
    PairedDevice paired;
    assert(ble::BleAddress::parse("01:02:03:04:05:06", paired.address));
    paired.adapterId = "ftms";
    paired.valid = true;
    central.advertise(paired.address, "KICKR", {0x1826}, {}, -65, {1, 32, 0});
    input.loop();
    input.setPairedDevice(paired);
    assert(input.scanAndConnect());
    const auto& plan = central.connects.back().plan;
    assert(plan.subscriptionCount == 3);
    assert(plan.subscriptions[2].characteristic == ble::Uuid::from16(0x2ada));
    assert(plan.subscriptions[2].ifPresent);
    central.pushEvent(ble::CentralEventType::ReadResult, 0, 0, {0, 0, 0, 0, 8, 0, 0, 0});
    central.pushEvent(ble::CentralEventType::Connected, 7);
    input.loop();
    assert(input.isConnected());
    assert(central.writes.size() == 1);

    central.pushEvent(ble::CentralEventType::Notification, 0, 1, {0x80, 0, 1});
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, {0x44, 0, 0x90, 1, 0xa0, 0, 0x96, 0});
    central.pushEvent(ble::CentralEventType::Notification, 0, 2, {2});
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, {0});
    central.pushEvent(ble::CentralEventType::Notification, 0, 99, {0xaa});
    input.loop();
    assert(input.telemetry().speed.value() == 4.0F);
    assert(input.telemetry().cadence.value() == 80.0F);
    assert(input.telemetry().power.value() == 150.0F);
    assert(central.writes.size() == 2);

    ble::CentralEvent old;
    old.type = ble::CentralEventType::Notification;
    old.session = central.lastSession() - 1;
    old.setData(bytes, sizeof(bytes));
    central.events.push(old);
    central.pushEvent(ble::CentralEventType::WriteFailed);
    input.loop();

    fake::now += 5001;
    input.loop();
    assert(input.stale());
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, {0, 0, 0x90, 1});
    input.loop();
    assert(!input.stale());
    input.disconnect();
    central.pushEvent(ble::CentralEventType::Notification, 0, 0, {0, 0, 0, 0});
    input.loop();

    // Optional missing characteristics compact the UI list, not event indices.
    testing::FakeCentral fallbackCentral;
    BleInputAdapter fallback(fallbackCentral, [] { return fake::now; });
    fallbackCentral.advertise(paired.address, "KICKR", {0x1826});
    fallback.loop();
    fallback.setPairedDevice(paired);
    assert(fallback.scanAndConnect());
    assert(fallbackCentral.connects.back().plan.subscriptionCount == 8);
    fallbackCentral.pushEvent(ble::CentralEventType::Connected, (1U << 0) | (1U << 4) | (1U << 7));
    fallback.loop();
    assert(fallback.subscriptions().size() == 3);
    fallbackCentral.pushEvent(ble::CentralEventType::Notification, 0, 4, {0x80, 0, 5});
    fallbackCentral.pushEvent(ble::CentralEventType::Notification, 0, 7, {1});
    fallback.loop();
    assert(fallback.isConnected());

    const auto text = debuglog::Hex(bytes, sizeof(bytes));
    SF_DEBUG_LOG("[test] %s\n", text.c_str());
    BridgeCore bridge;
    bridge.setTelemetry(input.telemetry());
    bridge.loop();
}
