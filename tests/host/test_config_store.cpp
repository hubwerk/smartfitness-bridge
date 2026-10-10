#include "bridge/bridge_core.hpp"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "config_backend.h"
#include "config_store.h"
#include "web_ui.h"

#include <cassert>
#include <string>
#include <vector>

using namespace smartfitness;

namespace {

BridgeConfig customConfig() {
    BridgeConfig config;
    config.pairedDeviceName = "Kickr";
    config.antEnabled = false;
    config.ftms = {true, false};
    config.ftmsName = "Garage Bike";
    config.simulatedTrainer = false;
    config.pairedSource.valid = true;
    config.pairedSource.addressType = 1;
    config.pairedSource.address = {0xC0, 0x11, 0x22, 0x33, 0x44, 0x55};
    config.pairedAdapterId = "ftms";
    config.wifiSsid = "Home WLAN";
    config.wifiPassword = "secret-pass";
    config.apPassword = "bridge-pass";
    return config;
}

std::vector<std::uint8_t> withCrc(std::vector<std::uint8_t> body) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : body) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    crc = ~crc;
    for (int shift = 0; shift < 32; shift += 8) body.push_back(static_cast<std::uint8_t>(crc >> shift));
    return body;
}

// A record exactly as written by schema version 1 firmware.
std::vector<std::uint8_t> v1Record() {
    std::vector<std::uint8_t> body{'S', 'F', 1, 0, 1, 0, 0};
    const std::string ftmsName = "Garage Bike";
    const std::string paired = "Kickr";
    body.push_back(static_cast<std::uint8_t>(ftmsName.size()));
    body.insert(body.end(), ftmsName.begin(), ftmsName.end());
    body.push_back(static_cast<std::uint8_t>(paired.size()));
    body.insert(body.end(), paired.begin(), paired.end());
    const std::vector<std::uint8_t> source{1, 1, 0xC0, 0x11, 0x22, 0x33, 0x44, 0x55};
    body.insert(body.end(), source.begin(), source.end());
    return withCrc(body);
}

void testMigrationFromV1() {
    BridgeConfig decoded;
    assert(ConfigStore::deserialize(v1Record(), decoded));
    BridgeConfig expected = customConfig();
    expected.pairedAdapterId.clear();
    expected.wifiSsid.clear();
    expected.wifiPassword.clear();
    expected.apPassword = BridgeConfig{}.apPassword;
    assert(decoded == expected);

    MemoryConfigBackend backend;
    backend.setRecord(v1Record());
    ConfigStore store(backend);
    assert(store.begin() == ConfigLoadResult::Loaded);
    assert(store.config() == expected);
    assert(!store.hasUnsavedChanges());
    // The first save rewrites the record in the current schema version.
    assert(store.save() == ConfigSaveResult::Written);
    assert(backend.record()[2] == ConfigStore::kSchemaVersion);
    assert(store.save() == ConfigSaveResult::Unchanged);
    ConfigStore rebooted(backend);
    assert(rebooted.begin() == ConfigLoadResult::Loaded && rebooted.config() == expected);

    // A v1-tagged record carrying v2 fields is rejected.
    std::vector<std::uint8_t> body(ConfigStore::serialize(customConfig()));
    body.resize(body.size() - 4);
    body[2] = 1;
    assert(!ConfigStore::deserialize(withCrc(body), decoded));
}

void testV2Limits() {
    BridgeConfig config;
    config.pairedAdapterId = std::string(16, 'a');
    config.wifiSsid = std::string(32, 's');
    config.wifiPassword = std::string(63, 'p');
    config.apPassword = std::string(63, 'q');
    config.ftmsName = std::string(20, 'f');
    config.pairedDeviceName = std::string(32, 'n');
    config.pairedSource.valid = true;
    const auto record = ConfigStore::serialize(config);
    assert(record.size() <= ConfigBackend::kMaxRecordSize);
    BridgeConfig decoded;
    assert(ConfigStore::deserialize(record, decoded) && decoded == config);

    assert(ConfigStore::isValidPassword("", true) && !ConfigStore::isValidPassword("", false));
    assert(!ConfigStore::isValidPassword("short", true));
    assert(!ConfigStore::isValidPassword(std::string(64, 'x'), true));
    assert(!ConfigStore::isValidAdapterId("FTMS") && !ConfigStore::isValidAdapterId(std::string(17, 'a')));
    assert(ConfigStore::isValidSsid("Caf\xC3\xA9") && !ConfigStore::isValidSsid("bad\nssid"));
    MemoryConfigBackend backend;
    ConfigStore store(backend);
    store.begin();
    store.config().apPassword = "";
    assert(store.save() == ConfigSaveResult::Invalid);
    store.config().apPassword = "longenough";
    store.config().wifiPassword = "1234567";
    assert(store.save() == ConfigSaveResult::Invalid);
}

void testRoundTripAcrossInstances() {
    MemoryConfigBackend backend;
    {
        ConfigStore store(backend);
        assert(store.begin() == ConfigLoadResult::Missing);
        assert(store.config() == BridgeConfig{});
        assert(!store.hasUnsavedChanges());
        store.config() = customConfig();
        assert(store.hasUnsavedChanges());
        assert(store.save() == ConfigSaveResult::Written);
        assert(!store.hasUnsavedChanges());
    }
    ConfigStore rebooted(backend);
    assert(rebooted.begin() == ConfigLoadResult::Loaded);
    assert(rebooted.config() == customConfig());
    assert(!rebooted.hasUnsavedChanges());

    // load discards unsaved edits and restores the stored record.
    rebooted.config().ftmsName = "Other";
    assert(rebooted.load() == ConfigLoadResult::Loaded);
    assert(rebooted.config().ftmsName == "Garage Bike");
}

void testNoWriteWhenUnchanged() {
    MemoryConfigBackend backend;
    ConfigStore store(backend);
    store.begin();
    // Defaults with no record are treated as already stored.
    assert(store.save() == ConfigSaveResult::Unchanged);
    assert(backend.writes() == 0);
    store.config().antEnabled = false;
    assert(store.save() == ConfigSaveResult::Written);
    assert(store.save() == ConfigSaveResult::Unchanged);
    assert(backend.writes() == 1);
    // Toggling back and forth without a net change does not write.
    store.config().antEnabled = true;
    store.config().antEnabled = false;
    assert(!store.hasUnsavedChanges());
    assert(store.save() == ConfigSaveResult::Unchanged);
    assert(backend.writes() == 1);

    ConfigStore again(backend);
    again.begin();
    assert(again.save() == ConfigSaveResult::Unchanged);
    assert(again.unpair());  // nothing paired: no write
    assert(backend.writes() == 1);

    backend.failWrites = true;
    again.config().ftmsName = "Fails";
    assert(again.save() == ConfigSaveResult::Failed);
    assert(again.hasUnsavedChanges());
}

void testCorruptAndOldVersionFallback() {
    const auto good = ConfigStore::serialize(customConfig());
    BridgeConfig decoded;
    assert(ConfigStore::deserialize(good, decoded) && decoded == customConfig());

    std::vector<std::vector<std::uint8_t>> bad;
    bad.push_back({});
    bad.push_back({0x01, 0x02, 0x03});
    auto flipped = good;
    flipped[8] ^= 0x01;  // CRC mismatch
    bad.push_back(flipped);
    auto truncated = good;
    truncated.pop_back();
    bad.push_back(truncated);

    // Re-encode raw fields with a fresh CRC to hit version and value validation.
    std::vector<std::uint8_t> body(good.begin(), good.end() - 4);
    auto oldVersion = body;
    oldVersion[2] = 0;
    bad.push_back(withCrc(oldVersion));
    auto newVersion = body;
    newVersion[2] = ConfigStore::kSchemaVersion + 1;
    bad.push_back(withCrc(newVersion));
    auto badBoolean = body;
    badBoolean[3] = 2;
    bad.push_back(withCrc(badBoolean));
    auto badName = body;
    badName[8] = '\t';  // first ftmsName character
    bad.push_back(withCrc(badName));
    auto longName = body;
    longName[7] = 21;
    longName.insert(longName.begin() + 8, 10, 'x');
    bad.push_back(withCrc(longName));
    auto badAddressType = body;
    badAddressType[badAddressType.size() - 7] = 9;
    bad.push_back(withCrc(badAddressType));

    for (const auto& record : bad) {
        assert(!ConfigStore::deserialize(record, decoded));
        MemoryConfigBackend backend;
        backend.setRecord(record);
        ConfigStore store(backend);
        assert(store.begin() == ConfigLoadResult::Invalid);
        assert(store.config() == BridgeConfig{});
        // The invalid record is replaced even though the config equals the defaults.
        assert(store.save() == ConfigSaveResult::Written);
        ConfigStore rebooted(backend);
        assert(rebooted.begin() == ConfigLoadResult::Loaded);
    }
}

void testValidation() {
    assert(ConfigStore::isValidFtmsName(""));
    assert(ConfigStore::isValidFtmsName(std::string(20, 'x')));
    assert(!ConfigStore::isValidFtmsName(std::string(21, 'x')));
    assert(!ConfigStore::isValidFtmsName("bad\nname"));

    MemoryConfigBackend backend;
    ConfigStore store(backend);
    store.begin();
    store.config().ftmsName = std::string(21, 'x');
    assert(store.save() == ConfigSaveResult::Invalid);
    store.config().ftmsName = "ok";
    store.config().pairedSource.addressType = 1;  // set without valid flag
    assert(store.save() == ConfigSaveResult::Invalid);
    assert(backend.writes() == 0);

    store.pairDevice(std::string("Bad\x01Name") + std::string(40, 'y'));
    assert(store.pairedDeviceName().size() == ConfigStore::kMaxPairedNameLength);
    assert(store.pairedDeviceName().substr(0, 8) == "Bad?Name");
}

void testUnpairPersistence() {
    MemoryConfigBackend backend;
    ConfigStore store(backend);
    store.begin();
    store.config() = customConfig();
    store.save();
    store.config().antEnabled = true;  // unsaved edit stays unsaved
    assert(store.unpair());
    assert(!store.hasPairedDevice());
    assert(store.hasUnsavedChanges());

    ConfigStore rebooted(backend);
    rebooted.begin();
    assert(!rebooted.hasPairedDevice());
    assert(!rebooted.config().pairedSource.valid);
    assert(rebooted.config().pairedAdapterId.empty());
    assert(rebooted.config().wifiSsid == "Home WLAN");
    assert(!rebooted.config().antEnabled);
    assert(rebooted.config().ftmsName == "Garage Bike");
}

void testFactoryReset() {
    MemoryConfigBackend backend;
    ConfigStore store(backend);
    store.begin();
    store.config() = customConfig();
    store.save();
    assert(store.factoryReset());
    assert(!backend.hasRecord() && backend.erases() == 1);
    assert(store.config() == BridgeConfig{});
    assert(!store.hasUnsavedChanges());
    assert(store.save() == ConfigSaveResult::Unchanged);
    ConfigStore rebooted(backend);
    assert(rebooted.begin() == ConfigLoadResult::Missing);
}

void testConsolePersistence() {
    MemoryConfigBackend backend;
    {
        SimulatedDeviceAdapter adapter;
        AntOutput ant;
        FtmsOutput ftms;
        ConfigStore config(backend);
        BridgeCore bridge;
        WebUi ui(bridge, config, adapter, ant, ftms);
        config.begin();
        bridge.attach(adapter, ant, ftms);
        bridge.begin();
        ui.begin();
        assert(ui.status().find("unsaved=0") != std::string::npos);
        assert(ui.handleCommand("save") == "configuration unchanged; nothing written");
        ui.handleCommand("ftms-name Garage Bike");
        ui.handleCommand("ftms-output off");
        ui.handleCommand("scan");
        assert(ui.status().find("unsaved=1") != std::string::npos);
        assert(ui.handleCommand("save") == "configuration saved");
        assert(ui.status().find("unsaved=0") != std::string::npos);
        assert(backend.writes() == 1);
        ui.handleCommand("ftms-name Changed");
        assert(ui.handleCommand("load").find("configuration loaded") == 0);
        assert(ftms.deviceName() == "Garage Bike");
    }
    {
        // Reboot: persisted name and output toggle are applied on begin.
        SimulatedDeviceAdapter adapter;
        AntOutput ant;
        FtmsOutput ftms;
        ConfigStore config(backend);
        BridgeCore bridge;
        WebUi ui(bridge, config, adapter, ant, ftms);
        assert(config.begin() == ConfigLoadResult::Loaded);
        bridge.attach(adapter, ant, ftms);
        bridge.begin();
        ui.begin();
        assert(ftms.deviceName() == "Garage Bike");
        assert(!ftms.enabled() && !ftms.advertising());
        assert(config.hasPairedDevice());
        ui.handleCommand("unpair");
        assert(backend.writes() == 2);
        assert(ui.handleCommand("factory-reset").find("factory defaults restored") == 0);
        assert(ftms.deviceName() == FtmsOutput::kDefaultName);
        assert(ftms.enabled() && ftms.advertising());
        assert(bridge.state() == RuntimeState::Disconnected);
        assert(ui.handleCommand("load").find("no saved configuration") == 0);
    }
    MemoryConfigBackend corrupt;
    corrupt.setRecord({0xFF});
    ConfigStore config(corrupt);
    SimulatedDeviceAdapter adapter;
    AntOutput ant;
    FtmsOutput ftms;
    BridgeCore bridge;
    WebUi ui(bridge, config, adapter, ant, ftms);
    assert(ui.handleCommand("load").find("saved configuration invalid") == 0);
}

}  // namespace

void runConfigStoreTests() {
    testRoundTripAcrossInstances();
    testNoWriteWhenUnchanged();
    testCorruptAndOldVersionFallback();
    testValidation();
    testMigrationFromV1();
    testV2Limits();
    testUnpairPersistence();
    testFactoryReset();
    testConsolePersistence();
}
