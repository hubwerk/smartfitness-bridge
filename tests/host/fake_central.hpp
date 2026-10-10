#pragma once

// Scriptable CentralTransport for host tests.

#include <deque>
#include <vector>

#include "ble/central_transport.hpp"

namespace smartfitness {
namespace testing {

class FakeCentral final : public ble::CentralTransport {
public:
    struct ConnectCall {
        std::uint32_t session;
        ble::BleAddress address;
        ble::ConnectPlan plan;
    };

    bool begin() override { return true; }
    bool startScan(std::uint32_t durationMs, bool duplicates) override {
        ++scanStarts;
        if (!acceptScan) { ++commandDrops; return false; }
        scanning_ = true;
        lastScanDuration = durationMs;
        lastScanDuplicates = duplicates;
        return true;
    }
    bool stopScan() override {
        ++scanStops;
        if (!acceptStop) { ++commandDrops; return false; }
        scanning_ = false;
        return true;
    }
    bool scanning() const override { return scanning_; }
    bool connect(std::uint32_t session, const ble::BleAddress& address, const ble::ConnectPlan& plan) override {
        connects.push_back({session, address, plan});
        if (!acceptConnect) { ++commandDrops; return false; }
        return true;
    }
    bool disconnect(std::uint32_t session) override {
        disconnects.push_back(session);
        if (!acceptDisconnect) { ++commandDrops; return false; }
        return true;
    }
    bool write(std::uint32_t, const ble::GattWrite& write) override {
        writes.push_back(write);
        if (!acceptWrite) { ++commandDrops; return false; }
        return true;
    }
    bool pollScan(ble::ScanReport& report) override {
        if (reports.empty()) return false;
        report = reports.front();
        reports.pop_front();
        return true;
    }
    bool pollEvent(ble::CentralEvent& event) override {
        return events.pop(event);
    }
    ble::BleAddress ownAddress() const override { return own; }
    std::uint32_t droppedScanReports() const override { return scanDrops; }
    std::uint32_t droppedEvents() const override { return events.dropped(); }
    std::uint32_t droppedNotifications() const override { return events.droppedNotifications(); }
    std::uint32_t droppedLifecycleEvents() const override { return events.droppedLifecycleEvents(); }
    std::uint32_t droppedCommands() const override { return commandDrops; }

    std::uint32_t lastSession() const { return connects.empty() ? 0 : connects.back().session; }

    void pushEvent(ble::CentralEventType type, std::uint32_t mask = 0, std::uint8_t index = 0,
                   const ble::Bytes& data = {}) {
        if (type == ble::CentralEventType::ScanComplete) scanning_ = false;
        ble::CentralEvent event;
        event.type = type;
        event.session = lastSession();
        event.mask = mask;
        event.index = index;
        event.setData(data.data(), data.size());
        events.push(event);
    }

    void advertise(const ble::BleAddress& address, const std::string& name, std::vector<std::uint16_t> services = {},
                   const ble::Bytes& manufacturer = {}, int rssi = -60) {
        ble::ScanReport report;
        report.address = address;
        report.setName(name);
        report.rssi = static_cast<std::int8_t>(rssi);
        for (auto service : services) report.addService(ble::Uuid::from16(service));
        report.setManufacturerData(manufacturer.data(), manufacturer.size());
        reports.push_back(report);
    }

    bool acceptScan = true;
    bool acceptStop = true;
    bool acceptDisconnect = true;
    bool acceptConnect = true;
    bool acceptWrite = true;
    bool scanning_ = false;
    int scanStarts = 0;
    int scanStops = 0;
    std::uint32_t lastScanDuration = 0;
    bool lastScanDuplicates = false;
    ble::BleAddress own;
    std::vector<ConnectCall> connects;
    std::vector<std::uint32_t> disconnects;
    std::vector<ble::GattWrite> writes;
    std::deque<ble::ScanReport> reports;
    ble::CentralEventQueue<32> events;
    std::uint32_t scanDrops = 0;
    std::uint32_t commandDrops = 0;
};

inline ble::BleAddress address(const char* text) {
    ble::BleAddress out;
    ble::BleAddress::parse(text, out);
    return out;
}

}  // namespace testing
}  // namespace smartfitness
