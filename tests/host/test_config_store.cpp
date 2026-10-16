#include "bridge/bridge_core.hpp"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "config_backend.h"
#include "config_store.h"
#include "bridge_commands.h"
#include "bridge_controller.h"

#include <cassert>
#include <string>
#include <vector>

using namespace smartfitness;

namespace {

BridgeConfig customConfig() {
    BridgeConfig config;
    config.pairedDeviceName = "Kickr";
    config.ftmsInput = true;
    config.outputProtocol = OutputProtocol::Off;
    config.ftmsName = "Garage Bike";
    config.simulatedTrainer = false;
    config.pairedSource.valid = true;
    config.pairedSource.addressType = 1;
    config.pairedSource.address = {0xC0, 0x11, 0x22, 0x33, 0x44, 0x55};
    config.pairedAdapterId = "ftms";
    config.wifiSsid = "Home WLAN";
    config.wifiPassword = "secret-pass";
    config.apPassword = "bridge-pass";
    config.ftmsOutputProfile = FtmsOutputProfile::Treadmill;
    config.httpsEnabled = false;
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

// A record as written by schema version 3 firmware: four leading flags
// (antEnabled ftmsInput ftmsOutput simulatedTrainer) and the profile byte.
std::vector<std::uint8_t> v3Record(bool ftmsOutput) {
    std::vector<std::uint8_t> body{'S', 'F', 3, 1, 0, static_cast<std::uint8_t>(ftmsOutput), 1};
    for (const std::string text : {"Garage Bike", "Kickr", "ftms", "Home", "pw-12345", "bridge-pass"}) {
        body.push_back(static_cast<std::uint8_t>(text.size()));
        body.insert(body.end(), text.begin(), text.end());
    }
    const std::vector<std::uint8_t> source{1, 1, 0xC0, 0x11, 0x22, 0x33, 0x44, 0x55};
    body.insert(body.end(), source.begin(), source.end());
    body.push_back(static_cast<std::uint8_t>(FtmsOutputProfile::Rower));
    return withCrc(body);
}

std::vector<std::uint8_t> v4Record(bool ftmsOutput, bool httpsEnabled) {
    auto body = v3Record(ftmsOutput);
    body.resize(body.size() - 4);
    body[2] = 4;
    body.push_back(static_cast<std::uint8_t>(httpsEnabled));
    return withCrc(body);
}

void testMigrationFromV3() {
    BridgeConfig decoded;
    assert(ConfigStore::deserialize(v3Record(true), decoded));
    assert(decoded.outputProtocol == OutputProtocol::BleFtms && !decoded.ftmsInput && decoded.simulatedTrainer);
    assert(decoded.ftmsName == "Garage Bike" && decoded.pairedAdapterId == "ftms" && decoded.wifiSsid == "Home");
    assert(decoded.ftmsOutputProfile == FtmsOutputProfile::Rower && decoded.pairedSource.valid && decoded.httpsEnabled);
    // The former ANT placeholder flag never selects ANT+ FE-C.
    assert(ConfigStore::deserialize(v3Record(false), decoded));
    assert(decoded.outputProtocol == OutputProtocol::Off);

    MemoryConfigBackend backend;
    backend.setRecord(v3Record(true));
    ConfigStore store(backend);
    assert(store.begin() == ConfigLoadResult::Loaded && !store.hasUnsavedChanges());
    store.config().outputProtocol = OutputProtocol::AntFec;
    assert(store.save() == ConfigSaveResult::Written);
    assert(backend.record()[2] == ConfigStore::kSchemaVersion &&
           backend.record()[3] == static_cast<std::uint8_t>(OutputProtocol::AntFec));
    ConfigStore rebooted(backend);
    assert(rebooted.begin() == ConfigLoadResult::Loaded);
    assert(rebooted.config().outputProtocol == OutputProtocol::AntFec);
}

void testMigrationFromV4() {
    BridgeConfig decoded;
    assert(ConfigStore::deserialize(v4Record(true, false), decoded));
    assert(decoded.outputProtocol == OutputProtocol::BleFtms && !decoded.httpsEnabled);
    assert(ConfigStore::deserialize(v4Record(false, true), decoded));
    assert(decoded.outputProtocol == OutputProtocol::Off && decoded.httpsEnabled);
}

void testMigrationFromV1() {
    BridgeConfig decoded;
    assert(ConfigStore::deserialize(v1Record(), decoded));
    BridgeConfig expected = customConfig();
    expected.pairedAdapterId.clear();
    expected.wifiSsid.clear();
    expected.wifiPassword.clear();
    expected.apPassword = BridgeConfig{}.apPassword;
    expected.ftmsOutputProfile = FtmsOutputProfile::Auto;
    expected.httpsEnabled = true;
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

void testMigrationFromV2() {
    auto body = ConfigStore::serialize(customConfig());
    body.resize(body.size() - 6);  // Remove v3 profile, v4 HTTPS flag and CRC.
    body[2] = 2;
    // v2 header flags: antEnabled ftmsInput ftmsOutput simulatedTrainer.
    const std::vector<std::uint8_t> flags{1, body[4], 0, body[5]};
    body.erase(body.begin() + 3, body.begin() + 6);
    body.insert(body.begin() + 3, flags.begin(), flags.end());
    const auto legacy = withCrc(body);
    BridgeConfig expected = customConfig();
    expected.ftmsOutputProfile = FtmsOutputProfile::Auto;
    expected.httpsEnabled = true;
    MemoryConfigBackend backend;
    backend.setRecord(legacy);
    ConfigStore store(backend);
    assert(store.begin() == ConfigLoadResult::Loaded);
    assert(store.config() == expected && !store.hasUnsavedChanges());
    assert(backend.writes() == 0);
    assert(store.save() == ConfigSaveResult::Written);
    assert(backend.record()[2] == ConfigStore::kSchemaVersion);
    assert(store.save() == ConfigSaveResult::Unchanged);
    ConfigStore rebooted(backend);
    assert(rebooted.begin() == ConfigLoadResult::Loaded && rebooted.config() == expected);
}

void testOutputProfilePersistence() {
    assert(BridgeConfig{}.ftmsOutputProfile == FtmsOutputProfile::Auto);
    for (const auto profile : {FtmsOutputProfile::Auto, FtmsOutputProfile::IndoorBike,
                               FtmsOutputProfile::Treadmill, FtmsOutputProfile::CrossTrainer,
                               FtmsOutputProfile::Rower}) {
        MemoryConfigBackend backend;
        ConfigStore store(backend);
        store.begin();
        store.config() = customConfig();
        store.config().ftmsOutputProfile = profile;
        assert(store.save() == ConfigSaveResult::Written);
        ConfigStore rebooted(backend);
        assert(rebooted.begin() == ConfigLoadResult::Loaded);
        assert(rebooted.config() == store.config());
        assert(rebooted.config().ftmsOutputProfile == profile);
    }
    MemoryConfigBackend backend;
    ConfigStore store(backend);
    store.begin();
    store.config().ftmsOutputProfile = static_cast<FtmsOutputProfile>(255);
    assert(store.save() == ConfigSaveResult::Invalid);
    BridgeConfig decoded;
    assert(!ConfigStore::deserialize(ConfigStore::serialize(store.config()), decoded));
}

void testApPasswordValidationAndFallback() {
    const std::vector<std::string> invalid{
        "", "1234567", std::string(64, 'x'), "password\n", "password\t",
        std::string("password\0", 9), "password\x7f", "password\x80"};
    for (const auto& password : invalid) {
        assert(!ConfigStore::isValidApPassword(password));
        Command command;
        std::string error;
        assert(!parseConsoleCommand("ap-pass " + password, command, error));
        assert(error == "access point password must be 8-63 printable ASCII characters");

        BridgeConfig original = customConfig();
        original.apPassword = password;
        const auto record = ConfigStore::serialize(original);
        BridgeConfig expected = customConfig();
        expected.apPassword = BridgeConfig{}.apPassword;
        BridgeConfig decoded;
        assert(ConfigStore::deserialize(record, decoded) && decoded == expected);
        MemoryConfigBackend backend;
        backend.setRecord(record);
        ConfigStore store(backend);
        assert(store.begin() == ConfigLoadResult::Loaded);
        assert(store.config() == expected && !store.hasUnsavedChanges());
        assert(backend.writes() == 0);  // load does not wear flash
        assert(store.save() == ConfigSaveResult::Written);
        assert(store.save() == ConfigSaveResult::Unchanged);
        ConfigStore rebooted(backend);
        assert(rebooted.begin() == ConfigLoadResult::Loaded && rebooted.config() == expected);

        store.config().apPassword = password;
        assert(store.save() == ConfigSaveResult::Invalid);
        assert(backend.writes() == 1);
    }
    for (const auto& password : {std::string(8, ' '), std::string(63, '~'), std::string("with spaces!")}) {
        assert(ConfigStore::isValidApPassword(password));
        Command command;
        std::string error;
        assert(parseConsoleCommand("ap-pass " + password, command, error));
        assert(command.type == CommandType::ApPassword && command.text == password);
    }
    // Password recovery never bypasses CRC or validation of unrelated fields.
    BridgeConfig bad = customConfig();
    bad.apPassword.clear();
    bad.wifiPassword = "short";
    BridgeConfig decoded;
    assert(!ConfigStore::deserialize(ConfigStore::serialize(bad), decoded));
    bad.wifiPassword = customConfig().wifiPassword;
    auto corrupt = ConfigStore::serialize(bad);
    corrupt[3] ^= 1;
    assert(!ConfigStore::deserialize(corrupt, decoded));
}

void testNonFiniteScanDurations() {
    for (const char* value : {"nan", "inf", "-inf", "1e100"}) {
        Command command;
        std::string error;
        assert(!parseConsoleCommand(std::string("scan ") + value, command, error));
    }
}

void testHttpsConsoleCommand() {
    Command command;
    std::string error;
    assert(parseConsoleCommand("https off", command, error) && command.type == CommandType::Https && !command.flag);
    assert(parseConsoleCommand("https on", command, error) && command.type == CommandType::Https && command.flag);
    assert(!parseConsoleCommand("https maybe", command, error) && error == "expected on or off");
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
    store.config().outputProtocol = OutputProtocol::Off;
    assert(store.save() == ConfigSaveResult::Written);
    assert(store.save() == ConfigSaveResult::Unchanged);
    assert(backend.writes() == 1);
    // Toggling back and forth without a net change does not write.
    store.config().outputProtocol = OutputProtocol::AntFec;
    store.config().outputProtocol = OutputProtocol::Off;
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
    auto badProtocol = body;
    badProtocol[3] = kOutputProtocolCount;
    bad.push_back(withCrc(badProtocol));
    auto badBoolean = body;
    badBoolean[4] = 2;
    bad.push_back(withCrc(badBoolean));
    auto badName = body;
    badName[7] = '\t';  // first ftmsName character
    bad.push_back(withCrc(badName));
    auto longName = body;
    longName[6] = 21;
    longName.insert(longName.begin() + 7, 10, 'x');
    bad.push_back(withCrc(longName));
    auto badAddressType = body;
    badAddressType[badAddressType.size() - 9] = 9;
    bad.push_back(withCrc(badAddressType));
    auto badHttps = body;
    badHttps.back() = 2;
    bad.push_back(withCrc(badHttps));

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
    store.config().outputProtocol = OutputProtocol::AntFec;  // unsaved edit stays unsaved
    store.config().ftmsOutputProfile = FtmsOutputProfile::Rower;
    assert(store.unpair());
    assert(!store.hasPairedDevice());
    assert(store.hasUnsavedChanges());

    ConfigStore rebooted(backend);
    rebooted.begin();
    assert(!rebooted.hasPairedDevice());
    assert(!rebooted.config().pairedSource.valid);
    assert(rebooted.config().pairedAdapterId.empty());
    assert(rebooted.config().wifiSsid == "Home WLAN");
    assert(rebooted.config().outputProtocol == OutputProtocol::Off);
    assert(rebooted.config().ftmsOutputProfile == FtmsOutputProfile::Treadmill);
    assert(store.config().ftmsOutputProfile == FtmsOutputProfile::Rower);
    assert(rebooted.config().ftmsName == "Garage Bike");
}

void testFailedFactoryReset() {
    class FailingEraseBackend : public ConfigBackend {
    public:
        MemoryConfigBackend memory;
        bool read(std::vector<std::uint8_t>& data) override { return memory.read(data); }
        bool write(const std::vector<std::uint8_t>& data) override { return memory.write(data); }
        bool erase() override { return false; }
    } backend;
    ConfigStore store(backend);
    store.begin();
    store.config() = customConfig();
    assert(store.save() == ConfigSaveResult::Written);
    store.config().outputProtocol = OutputProtocol::AntFec;
    assert(!store.factoryReset());
    assert(store.config() == BridgeConfig{} && store.hasUnsavedChanges());
    assert(store.save() == ConfigSaveResult::Written);
    ConfigStore rebooted(backend);
    assert(rebooted.begin() == ConfigLoadResult::Loaded && rebooted.config() == BridgeConfig{});
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
        BridgeController ui(bridge, config, adapter, ant, ftms);
        config.begin();
        bridge.attach(adapter, ant, ftms);
        bridge.begin();
        ui.begin();
        assert(ui.status().find("unsaved=0") != std::string::npos);
        assert(ui.handleCommand("save") == "configuration unchanged; nothing written");
        ui.handleCommand("ftms-name Garage Bike");
        ui.handleCommand("ftms-profile treadmill");
        ui.handleCommand("output off");
        ui.handleCommand("scan");
        assert(ui.status().find("unsaved=1") != std::string::npos);
        assert(ui.handleCommand("save") == "configuration saved");
        assert(ui.status().find("unsaved=0") != std::string::npos);
        assert(backend.writes() == 1);
        ui.handleCommand("ftms-name Changed");
        ui.handleCommand("ftms-profile bike");
        assert(ui.handleCommand("load").find("configuration loaded") == 0);
        assert(ftms.deviceName() == "Garage Bike");
        assert(ftms.profile() == MachineProfile::Treadmill);
    }
    {
        // Reboot: persisted name and output toggle are applied on begin.
        SimulatedDeviceAdapter adapter;
        AntOutput ant;
        FtmsOutput ftms;
        ConfigStore config(backend);
        BridgeCore bridge;
        BridgeController ui(bridge, config, adapter, ant, ftms);
        assert(config.begin() == ConfigLoadResult::Loaded);
        bridge.attach(adapter, ant, ftms);
        bridge.begin();
        ui.begin();
        assert(ftms.deviceName() == "Garage Bike");
        assert(!ftms.enabled() && !ftms.advertising());
        assert(ftms.profile() == MachineProfile::Treadmill);
        assert(config.hasPairedDevice());
        ui.handleCommand("unpair");
        assert(backend.writes() == 2);
        assert(ui.handleCommand("factory-reset").find("factory defaults restored") == 0);
        assert(ftms.deviceName() == FtmsOutput::kDefaultName);
        assert(ftms.profile() == MachineProfile::IndoorBike);
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
    BridgeController ui(bridge, config, adapter, ant, ftms);
    assert(ui.handleCommand("load").find("saved configuration invalid") == 0);
}

}  // namespace

void runConfigStoreTests() {
    testRoundTripAcrossInstances();
    testNoWriteWhenUnchanged();
    testCorruptAndOldVersionFallback();
    testValidation();
    testMigrationFromV1();
    testMigrationFromV3();
    testMigrationFromV2();
    testMigrationFromV4();
    testOutputProfilePersistence();
    testV2Limits();
    testApPasswordValidationAndFallback();
    testNonFiniteScanDurations();
    testHttpsConsoleCommand();
    testUnpairPersistence();
    testFactoryReset();
    testFailedFactoryReset();
    testConsolePersistence();
}
