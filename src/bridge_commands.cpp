#include "bridge_commands.h"

#include <cstdlib>

using namespace smartfitness;

namespace {
bool parseOnOff(const std::string& value, bool& out) {
    if (value == "on") out = true;
    else if (value == "off") out = false;
    else return false;
    return true;
}

bool parseFloat(const std::string& value, float& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    out = std::strtof(value.c_str(), &end);
    return end != value.c_str() && end == value.c_str() + value.size();
}

// Splits off the first space separated word.
std::string nextWord(std::string& rest) {
    const auto separator = rest.find(' ');
    std::string word = rest.substr(0, separator);
    rest = separator == std::string::npos ? std::string() : rest.substr(separator + 1);
    return word;
}
}  // namespace

bool parseControlName(const std::string& name, Control& control) {
    if (name == "power") control = Control::Power;
    else if (name == "speed") control = Control::Speed;
    else if (name == "incline") control = Control::Incline;
    else if (name == "resistance") control = Control::Resistance;
    else return false;
    return true;
}

const char* controlName(Control control) {
    switch (control) {
        case Control::Power: return "power";
        case Control::Speed: return "speed";
        case Control::Incline: return "incline";
        case Control::Resistance: return "resistance";
        case Control::Count: break;
    }
    return "unknown";
}

const char* controlResultName(ControlResult result) {
    switch (result) {
        case ControlResult::Applied: return "applied";
        case ControlResult::Disconnected: return "disconnected";
        case ControlResult::NotTrainer: return "not a trainer";
        case ControlResult::Unsupported: return "unsupported control";
        case ControlResult::InvalidValue: return "invalid value";
        case ControlResult::NoAdapter: return "no adapter";
        case ControlResult::AdapterRejected: return "adapter rejected";
    }
    return "unknown";
}

bool parseConsoleCommand(const std::string& line, Command& command, std::string& error) {
    command = Command{};
    if (line.size() > kMaxCommandLength) {
        error = "command too long";
        return false;
    }
    struct Simple {
        const char* name;
        CommandType type;
    };
    static const Simple kSimple[] = {
        {"help", CommandType::Help},          {"status", CommandType::Status},
        {"sim", CommandType::Simulate},       {"scan", CommandType::Scan},
        {"scan-stop", CommandType::ScanStop}, {"scan-results", CommandType::ScanResults},
        {"connect", CommandType::Connect},    {"disconnect", CommandType::Disconnect},
        {"unpair", CommandType::Unpair},      {"save", CommandType::Save},
        {"load", CommandType::Load},          {"factory-reset", CommandType::FactoryReset},
        {"wifi-clear", CommandType::WifiClear}, {"restart", CommandType::Restart},
    };
    for (const auto& entry : kSimple) {
        if (line == entry.name) {
            command.type = entry.type;
            return true;
        }
    }
    error = "unknown command; type help";
    const auto separator = line.find(' ');
    if (separator == std::string::npos) return false;
    const std::string key = line.substr(0, separator);
    std::string value = line.substr(separator + 1);

    if (key == "ftms-name") {
        command.type = CommandType::FtmsName;
        command.text = value;
        return true;
    }
    if (key == "scan") {
        float seconds = 0.0F;
        if (!parseFloat(value, seconds) || seconds < 1.0F || seconds > 60.0F) {
            error = "expected scan duration 1-60 s";
            return false;
        }
        command.type = CommandType::Scan;
        command.durationMs = static_cast<std::uint32_t>(seconds * 1000.0F);
        return true;
    }
    if (key == "pair") {
        std::string rest = value;
        const std::string address = nextWord(rest);
        const std::string adapter = nextWord(rest);
        const std::string addressType = nextWord(rest);
        if (!rest.empty() || !ble::BleAddress::parse(address, command.address)) {
            error = "usage: pair <aa:bb:cc:dd:ee:ff> [adapter|auto] [public|random]";
            return false;
        }
        if (adapter != "auto") command.adapterId = adapter;
        if (addressType == "public" || addressType == "random") {
            command.address.type = addressType == "random" ? 1 : 0;
            command.addressTypeGiven = true;
        } else if (!addressType.empty()) {
            error = "address type must be public or random";
            return false;
        }
        command.type = CommandType::Pair;
        return true;
    }
    if (key == "wifi") {
        // Console form: wifi <ssid> [password]; SSIDs with spaces need the web UI.
        std::string rest = value;
        command.text = nextWord(rest);
        command.text2 = rest;
        command.type = CommandType::Wifi;
        if (command.text.empty()) {
            error = "usage: wifi <ssid> [password]";
            return false;
        }
        return true;
    }
    if (key == "ap-pass") {
        command.type = CommandType::ApPassword;
        command.text = value;
        return true;
    }

    struct Toggle {
        const char* name;
        CommandType type;
    };
    static const Toggle kToggles[] = {
        {"trainer", CommandType::Trainer},
        {"ant", CommandType::Ant},
        {"ftms-input", CommandType::FtmsInput},
        {"ftms-output", CommandType::FtmsOutput},
    };
    for (const auto& entry : kToggles) {
        if (key == entry.name) {
            if (!parseOnOff(value, command.flag)) {
                error = "expected on or off";
                return false;
            }
            command.type = entry.type;
            return true;
        }
    }
    if (!parseControlName(key, command.control)) return false;
    if (!parseFloat(value, command.value)) {
        error = "invalid value";
        return false;
    }
    command.type = CommandType::Control;
    return true;
}
