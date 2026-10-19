#include "bridge_controller.h"
#include "bt_device_adapter.h"
#include "ant_output.h"
#include "ftms_output.h"
#include "ble_input_adapter.h"
#include "protocols/registry.hpp"
#include "scan_label.h"
#include "bridge/output_services.hpp"
#include <algorithm>
#include <cstdio>
#include <optional>
#include <utility>
#ifdef ARDUINO
#include <Arduino.h>
#include "debug_serial.h"
#endif

using namespace smartfitness;

const char* smartfitness::runtimeStateName(RuntimeState state) {
    switch (state) {
        case RuntimeState::Disconnected: return "disconnected";
        case RuntimeState::Connected: return "connected/read-only";
        case RuntimeState::Trainer: return "trainer";
        case RuntimeState::ControlReady: return "control-ready";
    }
    return "unknown";
}

std::vector<AdapterView> smartfitness::adapterViews() {
    std::vector<AdapterView> adapters;
    adapters.reserve(protocols::protocolCount());
    for (std::size_t index = 0; index < protocols::protocolCount(); ++index) {
        const auto& info = protocols::protocolAt(index);
        AdapterView adapter;
        adapter.id = info.id;
        adapter.label = info.label;
        adapter.controllable = info.controllable;
        adapter.advertisementOnly = info.advertisementOnly;
        adapter.profile = info.profile;
        adapters.push_back(adapter);
    }
    return adapters;
}

namespace {
const char* loadResultName(ConfigLoadResult result) {
    switch (result) {
        case ConfigLoadResult::Loaded: return "configuration loaded";
        case ConfigLoadResult::Missing: return "no saved configuration; defaults loaded";
        case ConfigLoadResult::Invalid: return "saved configuration invalid; defaults loaded";
    }
    return "unknown";
}

CommandResult ok(const std::string& message) { return {true, message}; }
CommandResult fail(const std::string& message) { return {false, message}; }

std::string pairedUuidString(const ble::Uuid& uuid) {
    const auto text = uuid.toString();
    return uuid.isShort() ? text.substr(2) : text;
}

PairedSourceDevice toSource(const ble::BleAddress& address) {
    PairedSourceDevice source;
    source.valid = true;
    source.addressType = address.type;
    source.address = address.bytes;
    return source;
}
}  // namespace

BridgeController::BridgeController(BridgeCore& bridge, ConfigStore& config, SimulatedDeviceAdapter& adapter,
                                   AntOutput& ant, FtmsOutput& ftms)
    : bridge_(bridge), config_(config), adapter_(adapter), ant_(ant), ftms_(ftms) {}

void BridgeController::attachBleInput(InputSelector& selector, BleInputAdapter& ble) {
    selector_ = &selector;
    ble_ = &ble;
}

bool BridgeController::bleSelected() const {
    return selector_ && selector_->source() == InputSelector::Source::Ble;
}

void BridgeController::applyConfig() {
    const auto& config = config_.config();
    adapter_.setTrainer(config.simulatedTrainer);
    ftms_.setDeviceName(config.ftmsName);
    ftms_.setProfileOverride(config.ftmsOutputProfile);
    ant_.setProfileOverride(config.ftmsOutputProfile);
    ftms_.setServices(config.ftmsOutputServices);
    ant_.setServices(config.antOutputServices);
    // Exactly one output protocol is active; ANT+ FE-C needs an ANT radio.
    ftms_.setEnabled(config.outputProtocol == OutputProtocol::BleFtms);
    ant_.setEnabled(config.outputProtocol == OutputProtocol::AntFec && ant_.available());
#ifdef ARDUINO
    if (!outputProtocolLogged_ || lastOutputProtocol_ != config.outputProtocol) {
        DBG_SERIAL.printf("[BridgeCore] output protocol: %s\n", outputProtocolName(config.outputProtocol));
        lastOutputProtocol_ = config.outputProtocol;
        outputProtocolLogged_ = true;
    }
#endif
    if (ble_ && selector_) {
        const bool paired = config.pairedSource.valid && !config.pairedAdapterId.empty();
        if (paired) {
            PairedDevice device;
            device.valid = true;
            device.address.bytes = config.pairedSource.address;
            device.address.type = config.pairedSource.addressType;
            device.adapterId = config.pairedAdapterId;
            device.name = config.pairedDeviceName;
            for (const auto& service : config.pairedAdvertisedServices) {
                ble::Uuid uuid;
                if (ble::Uuid::parse(service, uuid)) device.advertisedServices.push_back(uuid);
            }
            ble_->setPairedDevice(device);
        } else if (ble_->pairedDevice().valid) {
            ble_->clearPairedDevice();
        }
        const auto source = config.ftmsInput && paired ? InputSelector::Source::Ble
                                                        : InputSelector::Source::Simulation;
#ifdef ARDUINO
        const auto previous = selector_->source();
#endif
        selector_->select(source);
#ifdef ARDUINO
        if (previous != selector_->source())
            DBG_SERIAL.printf("[BridgeCore] input source: %s\n",
                              selector_->source() == InputSelector::Source::Ble ? "BLE" : "simulation");
#endif
    }
    bridge_.loop();
}

void BridgeController::begin() {
    applyConfig();
    // Auto-connect the paired BLE device on boot.
    if (bleSelected()) bridge_.scanAndConnect();
#ifdef ARDUINO
    DBG_SERIAL.println(ble_ ? "[Console] Serial console. Type help." : "[Console] Serial console; simulation only. Type help.");
#endif
}

std::string BridgeController::status() const {
    char buffer[320];
    const auto& telemetry = bridge_.telemetry();
    // Values the device did not send are shown as "--", never as 0.
    char values[4][16];
    const std::optional<float>* fields[] = {&telemetry.speed, &telemetry.cadence, &telemetry.power, &telemetry.heartRate};
    for (std::size_t index = 0; index < 4; ++index) {
        if (*fields[index]) std::snprintf(values[index], sizeof(values[index]), "%.1f", static_cast<double>(**fields[index]));
        else std::snprintf(values[index], sizeof(values[index]), "--");
    }
    std::snprintf(buffer, sizeof(buffer),
                  "state=%s speed=%skm/h cadence=%srpm power=%sW hr=%sbpm "
                  "target=%s output=%s ftms-input=%d "
                  "ftms-out(advertising=%d,client=%d,control=%d) unsaved=%d",
                  runtimeStateName(bridge_.state()), values[0], values[1], values[2], values[3],
                  bridge_.hasControlTarget() ? "set" : "none",
                  outputProtocolName(config_.config().outputProtocol), config_.config().ftmsInput,
                  ftms_.advertising(), ftms_.clientConnected(), ftms_.controlGranted(), config_.hasUnsavedChanges());
    std::string text = buffer;
    text += std::string(" input-profile=") + machineProfileName(bridge_.trainerState().profile) +
            " output-profile=" + machineProfileName(ftms_.profile()) +
            " ftms-profile=" + ftmsOutputProfileName(config_.config().ftmsOutputProfile);
    if (config_.config().outputProtocol == OutputProtocol::AntFec)
        text += std::string(" ant(link=") + AntOutput::linkName(ant_.link()) +
                ",device=" + std::to_string(ant_.deviceNumber()) + ",display=" + std::to_string(ant_.displaySeen()) +
                ",control=" + std::to_string(ant_.controlActive()) + ")";
    if (ble_) {
        text += std::string(" input=") + (bleSelected() ? "ble" : "sim") +
                " link=" + BleInputAdapter::linkStateName(ble_->linkState());
        if (ble_->pairedDevice().valid)
            text += " paired=" + ble_->pairedDevice().address.toString() + "/" + ble_->pairedDevice().adapterId;
        if (ble_->stale()) text += " stale=1";
        if (!ble_->lastError().empty()) text += " error=\"" + ble_->lastError() + "\"";
        text += " dropped(scans=" + std::to_string(ble_->droppedScanReports()) +
                ",events=" + std::to_string(ble_->droppedEvents()) +
                ",notifications=" + std::to_string(ble_->droppedNotifications()) +
                ",commands=" + std::to_string(ble_->droppedCommands()) + ")";
    }
    return text;
}

std::string BridgeController::handleCommand(const std::string& line) {
    Command command;
    std::string error;
    if (!parseConsoleCommand(line, command, error)) return error;
    return execute(command).message;
}

CommandResult BridgeController::pair(const Command& command) {
    if (!ble_ || !selector_) return fail("BLE input not available");
    if (ble_->isExcluded(command.address)) return fail("refusing to pair with the bridge itself or its FTMS client");
    PairedDevice device;
    device.valid = true;
    device.address = command.address;
    const ScanResult* result = ble_->findResult(command.address);
    if (result) {
        if (!command.addressTypeGiven) device.address.type = result->address.type;
        device.name = result->name;
    }
    device.adapterId = command.adapterId.empty() ? (result ? result->detectedAdapter : std::string()) : command.adapterId;
    if (device.adapterId.empty())
        return fail(result ? "device type not detected; choose an adapter" : "device not in scan results; choose an adapter");
    if (!protocols::findProtocol(device.adapterId)) return fail("unknown adapter: " + device.adapterId);
    std::vector<std::string> advertisedServices;
    if (result) {
        for (const auto& uuid : result->services) {
            if (advertisedServices.size() >= ble::kMaxAdvertisedServices) break;
            advertisedServices.push_back(pairedUuidString(uuid));
        }
    }
    if (!config_.pairSource(toSource(device.address), device.name, device.adapterId, advertisedServices))
        return fail("pairing not persisted (storage write failed)");
    applyConfig();
#ifdef ARDUINO
    DBG_SERIAL.printf("[BLE] paired %s (%s) adapter=%s\n", device.name.empty() ? "device" : device.name.c_str(),
                      device.address.toString().c_str(), device.adapterId.c_str());
#endif
    if (!ble_->scanAndConnect()) return fail("paired, but connection request rejected: " + ble_->lastError());
    bridge_.loop();
    const std::string label = device.name.empty() ? device.address.toString() : device.name + " (" + device.address.toString() + ")";
    return ok("paired " + label + " as " + device.adapterId + "; connecting");
}

CommandResult BridgeController::execute(const Command& command) {
    switch (command.type) {
        case CommandType::Help:
            return ok("status | sim | scan [s] | scan-stop | scan-results | pair <addr> [adapter|auto] [public|random] | "
                      "connect | disconnect | unpair | trainer on/off | "
                      "power W | speed km/h | incline % | resistance % | "
                      "output off/ble-ftms/ant-fec | ftms-input on/off | ftms-name <name> | "
                      "ftms-profile auto/bike/treadmill/cross-trainer/rower/step-climber/stair-climber | "
                      "output-services ftms|ant auto|0..31 | "
                      "wifi <ssid> [password] | wifi-clear | ap-pass <password> | https on/off | "
                      "save | load | factory-reset | restart");
        case CommandType::Status: return ok(status());
        case CommandType::Scan:
            if (ble_) {
                const std::uint32_t duration = command.durationMs ? command.durationMs : BleInputAdapter::kDefaultScanMs;
                if (!ble_->startScan(duration)) return fail("scan not started: " + ble_->lastError());
                char buffer[48];
                std::snprintf(buffer, sizeof(buffer), "scanning for %u s", static_cast<unsigned>(duration / 1000));
                return ok(buffer);
            }
            // Without BLE input "scan" keeps its original meaning: connect the simulation.
            [[fallthrough]];
        case CommandType::Simulate: {
            if (ble_ && config_.config().ftmsInput) {
                config_.config().ftmsInput = false;
                applyConfig();
            }
            if (!bridge_.scanAndConnect()) return fail("connection failed");
            if (!ble_) config_.pairDevice(bridge_.telemetry().deviceName);
            bridge_.loop();
            return ok("simulation connected; " + status());
        }
        case CommandType::ScanStop:
            if (!ble_) return fail("BLE input not available");
            if (!ble_->stopScan()) return fail("scan stop rejected: " + ble_->lastError());
            return ok("scan stopped");
        case CommandType::ScanResults: {
            if (!ble_) return fail("BLE input not available");
            std::string text = ble_->scanning() ? "scanning; " : "";
            char line[160];
            std::snprintf(line, sizeof(line), "%u device(s)", static_cast<unsigned>(ble_->scanResults().size()));
            text += line;
            for (const auto& result : ble_->scanResults()) {
                std::snprintf(line, sizeof(line), "\n  %s %s rssi=%d type=%s name=\"%s\"",
                              result.address.toString().c_str(), result.address.type ? "random" : "public",
                              result.rssi, result.detectedAdapter.empty() ? "?" : result.detectedAdapter.c_str(),
                              result.name.c_str());
                text += line;
            }
            return ok(text);
        }
        case CommandType::Pair: return pair(command);
        case CommandType::Connect:
            if (!ble_) return fail("BLE input not available");
            if (!config_.config().pairedSource.valid) return fail("no paired device; scan and pair first");
            config_.config().ftmsInput = true;
            applyConfig();
            if (!ble_->scanAndConnect()) return fail("connection request rejected: " + ble_->lastError());
            bridge_.loop();
            return ok("connecting; " + status());
        case CommandType::Disconnect:
        case CommandType::Unpair: {
            const bool unpair = command.type == CommandType::Unpair;
            const PairedDevice previous = ble_ ? ble_->pairedDevice() : PairedDevice{};
            const bool accepted = !bleSelected() || ble_->requestDisconnect();
            bridge_.disconnect();
            if (!accepted) return fail("disconnect request rejected: " + ble_->lastError());
            const bool persisted = !unpair || config_.unpair();
            if (unpair) {
                applyConfig();
#ifdef ARDUINO
                DBG_SERIAL.printf("[BLE] unpaired %s\n", previous.valid ? previous.address.toString().c_str() : "device");
#endif
            }
            bridge_.loop();
            return persisted ? ok(status()) : fail("unpair not persisted (storage write failed); " + status());
        }
        case CommandType::Save:
            switch (config_.save()) {
                case ConfigSaveResult::Written: return ok("configuration saved");
                case ConfigSaveResult::Unchanged: return ok("configuration unchanged; nothing written");
                case ConfigSaveResult::Invalid: return fail("configuration invalid; not saved");
                case ConfigSaveResult::Failed: return fail("save failed (storage write failed)");
            }
            return fail("save failed");
        case CommandType::Load: {
            const auto result = config_.load();
            applyConfig();
            return {result != ConfigLoadResult::Invalid, std::string(loadResultName(result)) + "; " + status()};
        }
        case CommandType::FactoryReset: {
            bridge_.disconnect();
            const bool erased = config_.factoryReset();
            applyConfig();
            return {erased, std::string(erased ? "factory defaults restored; " : "erase failed; defaults restored in RAM; ") +
                                status()};
        }
        case CommandType::FtmsName: {
            const auto& value = command.text;
            if (value.empty() || value.size() > FtmsOutput::kMaxNameLength) return fail("expected 1-20 characters");
            if (!ConfigStore::isValidFtmsName(value)) return fail("expected printable ASCII");
            config_.config().ftmsName = value;
            applyConfig();
            return ok("ftms name: " + ftms_.deviceName());
        }
        case CommandType::FtmsProfile: {
            FtmsOutputProfile profile;
            if (!parseFtmsOutputProfile(command.text, profile))
                return fail("profile must be auto, bike, treadmill, cross-trainer, rower, step-climber or stair-climber");
            config_.config().ftmsOutputProfile = profile;
            applyConfig();
            return ok(status());
        }
        case CommandType::OutputProtocol: {
            OutputProtocol protocol;
            if (!parseOutputProtocol(command.text, protocol)) return fail("output must be off, ble-ftms or ant-fec");
            if (protocol == OutputProtocol::AntFec && !ant_.available())
                return fail("ANT+ FE-C needs an ANT+ USB stick (web variant, USB-OTG port)");
            config_.config().outputProtocol = protocol;
            applyConfig();
            return ok(status());
        }
        case CommandType::SetOutputServices: {
            const bool ftms = command.text == "ftms";
            if (!ftms && command.text != "ant") return fail("output services must select ftms or ant");
            if (!ftms && !ant_.available()) return fail("ANT+ output is unavailable");
            if (!validOutputServices(command.serviceMask, ftms))
                return fail(ftms ? "FTMS services must be Auto or include bit 1; maximum mask is 31" :
                                   "ANT services must be Auto or a mask from 1 to 31");
            const auto profile = effectiveOutputProfile(config_.config().ftmsOutputProfile,
                                                        bridge_.trainerState().profile);
            auto& config = config_.config();
            const auto previous = ftms ? config.ftmsOutputServices : config.antOutputServices;
            const auto added = effectiveOutputServices(command.serviceMask) & ~effectiveOutputServices(previous);
            if ((added & availableOutputServices(bridge_.trainerState(), profile, !ftms)) != added)
                return fail("selected services require unavailable source metrics or a compatible output profile");
            (ftms ? config.ftmsOutputServices : config.antOutputServices) = command.serviceMask;
            applyConfig();
            return ok(ftms && ftms_.restartRequired() ? "FTMS services selected; save and restart to apply" : status());
        }
        case CommandType::Trainer:
        case CommandType::FtmsInput: {
            auto& config = config_.config();
            bool* setting = command.type == CommandType::Trainer ? &config.simulatedTrainer : &config.ftmsInput;
            *setting = command.flag;
            applyConfig();
            return ok(status());
        }
        case CommandType::Wifi:
            if (command.text.empty() || !ConfigStore::isValidSsid(command.text)) return fail("expected SSID of 1-32 bytes");
            if (!ConfigStore::isValidPassword(command.text2, true))
                return fail("password must be empty or 8-63 printable characters");
            config_.config().wifiSsid = command.text;
            if (!command.flag) config_.config().wifiPassword = command.text2;  // flag: keep the stored password
            return ok("wifi: " + command.text + "; save and restart to apply");
        case CommandType::WifiClear:
            config_.config().wifiSsid.clear();
            config_.config().wifiPassword.clear();
            return ok("wifi cleared (access point only); save and restart to apply");
        case CommandType::ApPassword:
            if (!ConfigStore::isValidApPassword(command.text))
                return fail("access point password must be 8-63 printable ASCII characters");
            config_.config().apPassword = command.text;
            return ok("access point password set; save and restart to apply");
        case CommandType::Https:
            config_.config().httpsEnabled = command.flag;
            return ok(std::string("https ") + (command.flag ? "on" : "off") + "; save and restart to apply");
        case CommandType::Restart:
            restartRequested_ = true;
            return ok("restarting");
        case CommandType::Control: {
            const auto result = bridge_.routeControl({command.control, command.value});
            bridge_.loop();
            return {result == ControlResult::Applied, std::string("control: ") + controlResultName(result)};
        }
    }
    return fail("unknown command; type help");
}

StatusView BridgeController::statusView() const {
    StatusView view;
    view.version = SMARTFITNESS_VERSION;
    view.state = bridge_.state();
    const auto& state = bridge_.trainerState();
    for (std::size_t index = 0; index < view.telemetry.size(); ++index) {
        const auto& sample = state.sample(static_cast<Metric>(index));
        view.telemetry[index].valid = sample.valid;
        view.telemetry[index].value = sample.value;
    }
    view.deviceName = bridge_.telemetry().deviceName;
    view.trainer = state.trainerDetected;
    view.canControl = bridge_.canControl();
    for (std::size_t index = 0; index < view.controls.size(); ++index)
        view.controls[index] = state.capabilities.supports(static_cast<Control>(index));
    view.hasTarget = bridge_.hasControlTarget();
    if (view.hasTarget) view.target = bridge_.controlTarget();
    auto& input = view.input;
    input.bleSelected = bleSelected();
    input.profile = state.profile;
    input.bleAvailable = ble_ != nullptr;
    if (ble_) {
        input.link = BleInputAdapter::linkStateName(ble_->linkState());
        input.protocol = ble_->protocolId();
        input.stale = ble_->stale();
        input.scanning = ble_->scanning();
        input.error = ble_->lastError();
        input.reconnectAttempts = ble_->reconnectAttempts();
        input.nextAttemptMs = ble_->nextAttemptInMs();
        input.autoConnect = ble_->autoConnect();
        input.dropped.scans = ble_->droppedScanReports();
        input.dropped.events = ble_->droppedEvents();
        input.dropped.notifications = ble_->droppedNotifications();
        input.dropped.commands = ble_->droppedCommands();
    }
    const auto& config = config_.config();
    if (config.pairedSource.valid) {
        ble::BleAddress address;
        address.bytes = config.pairedSource.address;
        input.paired.valid = true;
        input.paired.address = address.toString();
        input.paired.randomAddress = config.pairedSource.addressType != 0;
        input.paired.name = config.pairedDeviceName;
        input.paired.adapter = config.pairedAdapterId;
        if (ble_) {
            for (const auto& uuid : ble_->advertisedServices())
                input.paired.advertisedServices.push_back(pairedUuidString(uuid));
            for (const auto& subscription : ble_->subscriptions()) {
                SubscriptionView item;
                item.service = pairedUuidString(subscription.service);
                item.characteristic = pairedUuidString(subscription.characteristic);
                item.state = subscription.state;
                item.indicate = subscription.indicate;
                item.reason = subscription.reason;
                input.paired.subscriptions.push_back(std::move(item));
            }
        }
    }
    view.outputProtocol = config.outputProtocol;
    auto& ftms = view.ftmsOutput;
    ftms.enabled = ftms_.enabled();
    ftms.profile = ftms_.profile();
    ftms.override = config.ftmsOutputProfile;
    ftms.name = ftms_.deviceName();
    ftms.advertising = ftms_.advertising();
    ftms.client = ftms_.clientConnected();
    std::array<std::uint8_t, 6> clientBytes{};
    std::uint8_t clientType = 0;
    if (ftms.client && ftms_.clientAddress(clientBytes, clientType)) {
        ble::BleAddress client;
        client.bytes = clientBytes;
        ftms.clientAddress = client.toString();
        ftms.clientRandomAddress = clientType != 0;
    }
    ftms.control = ftms_.controlGranted();
    ftms.restartRequired = ftms_.restartRequired();
    auto& ant = view.antOutput;
    ant.enabled = ant_.enabled();
    ant.available = ant_.available();
    ant.link = AntOutput::linkName(ant_.link());
    ant.stick = ant_.stickConnected();
    ant.open = ant_.channelOpen();
    ant.deviceNumber = ant_.deviceNumber();
    ant.profile = ant_.profile();
    ant.display = ant_.displaySeen();
    ant.control = ant_.controlActive();
    ant.hardware = ant_.hardware();
    ant.error = AntOutput::errorName(ant_.error());
    ant.channelLimit = ant_.channelLimit();
    const auto available = availableOutputServices(state, ftms.profile);
    const auto antAvailable = availableOutputServices(state, ant.profile, true);
    for (std::uint8_t bit = 1; bit <= kOutputServiceRunningSpeedCadence; bit <<= 1) {
        OutputServiceView service;
        service.id = outputServiceId(bit);
        service.name = outputServiceName(bit);
        service.enabled = (effectiveOutputServices(config.ftmsOutputServices) & bit) != 0;
        service.available = (available & bit) != 0;
        service.active = ftms_.serviceActive(bit);
        ftms.services.push_back(std::move(service));
        AntChannelView channel;
        channel.id = outputServiceId(bit, true);
        channel.name = outputServiceName(bit, true);
        channel.deviceType = antOutputDeviceType(bit);
        channel.enabled = (effectiveOutputServices(config.antOutputServices) & bit) != 0;
        channel.open = ant_.channelOpen(bit);
        channel.available = (antAvailable & bit) != 0;
        ant.channels.push_back(std::move(channel));
    }
    view.unsaved = config_.hasUnsavedChanges();
    return view;
}

ScanView BridgeController::scanView() const {
    ScanView view;
    view.scanning = ble_ && ble_->scanning();
    view.bleAvailable = ble_ != nullptr;
    view.dropped = ble_ ? ble_->droppedScanResults() : 0U;
    if (ble_) {
        view.devices.reserve(ble_->scanResults().size());
        for (const auto& result : ble_->scanResults()) {
            ScanDeviceView device;
            device.address = result.address.toString();
            device.randomAddress = result.address.type != 0;
            device.name = result.name;
            device.rssi = result.rssi;
            device.connectable = result.connectable;
            device.adapter = result.detectedAdapter;
            for (const auto& uuid : result.services) device.services.push_back(uuid.toString());
            device.label = scanDeviceLabel(result.name, result.manufacturerData, result.services, result.address);
            device.relevant = isRelevantDevice(result.name, result.detectedAdapter, result.services);
            view.devices.push_back(std::move(device));
        }
        auto score = [](const ScanDeviceView& d) -> int {
            if (!d.adapter.empty()) return 2;
            if (d.relevant) return 1;
            return 0;
        };
        std::sort(view.devices.begin(), view.devices.end(), [&score](const ScanDeviceView& a, const ScanDeviceView& b) {
            const int scoreA = score(a);
            const int scoreB = score(b);
            if (scoreA != scoreB) return scoreA > scoreB;
            return a.rssi > b.rssi;
        });
    }
    return view;
}

ConfigView BridgeController::configView() const {
    const auto& config = config_.config();
    ConfigView view;
    view.version = SMARTFITNESS_VERSION;
    view.ftmsName = config.ftmsName.empty() ? std::string(FtmsOutput::kDefaultName) : config.ftmsName;
    view.outputProtocol = config.outputProtocol;
    view.antAvailable = ant_.available();
    view.ftmsOutputProfile = config.ftmsOutputProfile;
    view.ftmsInput = config.ftmsInput;
    view.simulatedTrainer = config.simulatedTrainer;
    view.wifiSsid = config.wifiSsid;
    view.wifiPasswordSet = !config.wifiPassword.empty();
    view.apPasswordDefault = config.apPassword == BridgeConfig{}.apPassword;
    view.https = config.httpsEnabled;
    view.unsaved = config_.hasUnsavedChanges();
    view.ftmsOutputServices = config.ftmsOutputServices;
    view.antOutputServices = config.antOutputServices;
    return view;
}

void BridgeController::loop() {
#ifdef ARDUINO
    // Bound work and line length so a noisy console cannot starve the bridge.
    for (unsigned int count = 0; count < 64 && Serial.available(); ++count) {
        const char character = static_cast<char>(Serial.read());
        if (character == '\r') continue;
        if (character == '\n') {
            if (overflow_) DBG_SERIAL.println("[Console] command too long");
            else if (!input_.empty()) DBG_SERIAL.println(handleCommand(input_).c_str());
            input_.clear();
            overflow_ = false;
        } else if (!overflow_) {
            if (input_.size() < kMaxCommandLength) input_ += character;
            else overflow_ = true;
        }
    }
#endif
}
