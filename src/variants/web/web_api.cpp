#include "web_api.h"

#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>

#include "bridge_controller.h"

using namespace smartfitness;

namespace {
constexpr std::size_t kMaxTextField = 64;

bool onlyKeys(JsonObjectConst object, std::initializer_list<const char*> allowed, std::string& error) {
    for (JsonPairConst pair : object) {
        bool known = false;
        for (const char* key : allowed) {
            if (pair.key() == key) known = true;
        }
        if (!known) {
            error = std::string("unknown field: ") + pair.key().c_str();
            return false;
        }
    }
    return true;
}

// Optional typed fields: return false only for a present value of the wrong type.
bool optionalText(JsonObjectConst object, const char* key, std::string& out, bool& present, std::string& error) {
    JsonVariantConst value = object[key];
    present = !value.isNull();
    if (!present) return true;
    if (!value.is<const char*>()) {
        error = std::string(key) + " must be a string";
        return false;
    }
    const JsonString text = value.as<JsonString>();
    if (std::memchr(text.c_str(), '\0', text.size())) {
        error = std::string(key) + " must not contain NUL";
        return false;
    }
    out.assign(text.c_str(), text.size());
    if (out.size() > kMaxTextField) {
        error = std::string(key) + " too long";
        return false;
    }
    return true;
}

bool optionalBool(JsonObjectConst object, const char* key, bool& out, bool& present, std::string& error) {
    JsonVariantConst value = object[key];
    present = !value.isNull();
    if (!present) return true;
    if (!value.is<bool>()) {
        error = std::string(key) + " must be true or false";
        return false;
    }
    out = value.as<bool>();
    return true;
}

Command simple(CommandType type) {
    Command command;
    command.type = type;
    return command;
}

std::string resultJson(std::uint32_t id, bool done, const CommandResult& result) {
    JsonDocument document;
    document["type"] = "result";
    document["id"] = id;
    document["done"] = done;
    if (done) {
        document["ok"] = result.ok;
        document["message"] = result.message;
    }
    std::string out;
    serializeJson(document, out);
    return out;
}

std::string errorJson(const std::string& message) {
    JsonDocument document;
    document["error"] = message;
    std::string out;
    serializeJson(document, out);
    return out;
}
}  // namespace

bool parseApiRequest(const std::string& endpoint, const std::string& body, std::vector<Command>& commands,
                     std::string& error) {
    commands.clear();
    JsonDocument document;
    if (body.empty()) {
        document.to<JsonObject>();
    } else {
        // Nesting limit keeps hostile bodies cheap to reject.
        const auto status = deserializeJson(document, body, DeserializationOption::NestingLimit(2));
        if (status) {
            error = std::string("invalid JSON: ") + status.c_str();
            return false;
        }
    }
    if (!document.is<JsonObject>()) {
        error = "expected a JSON object";
        return false;
    }
    JsonObjectConst object = document.as<JsonObjectConst>();

    struct NoBody {
        const char* endpoint;
        CommandType type;
    };
    static const NoBody kNoBody[] = {
        {"unpair", CommandType::Unpair},   {"connect", CommandType::Connect},
        {"disconnect", CommandType::Disconnect}, {"simulate", CommandType::Simulate},
        {"save", CommandType::Save},       {"load", CommandType::Load},
        {"factory-reset", CommandType::FactoryReset}, {"restart", CommandType::Restart},
    };
    for (const auto& entry : kNoBody) {
        if (endpoint == entry.endpoint) {
            if (!onlyKeys(object, {}, error)) return false;
            commands.push_back(simple(entry.type));
            return true;
        }
    }

    if (endpoint == "scan") {
        if (!onlyKeys(object, {"action", "seconds"}, error)) return false;
        std::string action = "start";
        bool present = false;
        if (!optionalText(object, "action", action, present, error)) return false;
        if (action == "stop") {
            commands.push_back(simple(CommandType::ScanStop));
            return true;
        }
        if (action != "start") {
            error = "action must be start or stop";
            return false;
        }
        Command command = simple(CommandType::Scan);
        JsonVariantConst seconds = object["seconds"];
        if (!seconds.isNull()) {
            if (!seconds.is<float>() || seconds.as<float>() < 1.0F || seconds.as<float>() > 60.0F) {
                error = "seconds must be a number between 1 and 60";
                return false;
            }
            command.durationMs = static_cast<std::uint32_t>(seconds.as<float>() * 1000.0F);
        }
        commands.push_back(command);
        return true;
    }

    if (endpoint == "pair") {
        if (!onlyKeys(object, {"address", "adapter", "addressType"}, error)) return false;
        Command command = simple(CommandType::Pair);
        std::string address, adapter, addressType;
        bool hasAddress = false, hasAdapter = false, hasType = false;
        if (!optionalText(object, "address", address, hasAddress, error) ||
            !optionalText(object, "adapter", adapter, hasAdapter, error) ||
            !optionalText(object, "addressType", addressType, hasType, error))
            return false;
        if (!hasAddress || !ble::BleAddress::parse(address, command.address)) {
            error = "address must look like aa:bb:cc:dd:ee:ff";
            return false;
        }
        if (adapter != "auto") command.adapterId = adapter;
        if (hasType) {
            if (addressType != "public" && addressType != "random") {
                error = "addressType must be public or random";
                return false;
            }
            command.address.type = addressType == "random" ? 1 : 0;
            command.addressTypeGiven = true;
        }
        commands.push_back(command);
        return true;
    }

    if (endpoint == "control") {
        if (!onlyKeys(object, {"control", "value"}, error)) return false;
        Command command = simple(CommandType::Control);
        std::string name;
        bool present = false;
        if (!optionalText(object, "control", name, present, error)) return false;
        if (!present || !parseControlName(name, command.control)) {
            error = "control must be power, resistance, incline or speed";
            return false;
        }
        JsonVariantConst value = object["value"];
        if (!value.is<float>()) {
            error = "value must be a number";
            return false;
        }
        command.value = value.as<float>();
        commands.push_back(command);
        return true;
    }

    if (endpoint == "config") {
        if (!onlyKeys(object,
                      {"ftmsName", "ftmsOutputProfile", "outputProtocol", "ftmsInput", "simulatedTrainer",
                       "wifiSsid", "wifiPassword", "apPassword", "https", "save"},
                      error))
            return false;
        std::string text;
        bool present = false;
        if (!object["ftmsOutputProfile"].isUnbound()) {
            FtmsOutputProfile profile;
            if (!object["ftmsOutputProfile"].is<const char*>()) {
                error = "ftmsOutputProfile must be a string";
                return false;
            }
            if (!optionalText(object, "ftmsOutputProfile", text, present, error)) return false;
            if (!parseFtmsOutputProfile(text, profile)) {
                error = "profile must be auto, bike, treadmill, cross-trainer, rower, step-climber or stair-climber";
                return false;
            }
            Command command = simple(CommandType::FtmsProfile);
            command.text = text;
            commands.push_back(command);
        }
        if (!optionalText(object, "ftmsName", text, present, error)) return false;
        if (present) {
            Command command = simple(CommandType::FtmsName);
            command.text = text;
            commands.push_back(command);
        }
        if (!optionalText(object, "outputProtocol", text, present, error)) return false;
        if (present) {
            OutputProtocol protocol;
            if (!parseOutputProtocol(text, protocol)) {
                error = "outputProtocol must be off, ble-ftms or ant-fec";
                return false;
            }
            Command command = simple(CommandType::OutputProtocol);
            command.text = text;
            commands.push_back(command);
        }
        struct Toggle {
            const char* key;
            CommandType type;
        };
        static const Toggle kToggles[] = {{"ftmsInput", CommandType::FtmsInput},
                                          {"simulatedTrainer", CommandType::Trainer}};
        for (const auto& toggle : kToggles) {
            Command command = simple(toggle.type);
            if (!optionalBool(object, toggle.key, command.flag, present, error)) return false;
            if (present) commands.push_back(command);
        }
        std::string ssid, password;
        bool hasSsid = false, hasPassword = false;
        if (!optionalText(object, "wifiSsid", ssid, hasSsid, error) ||
            !optionalText(object, "wifiPassword", password, hasPassword, error))
            return false;
        if (hasPassword && !hasSsid) {
            error = "wifiPassword requires wifiSsid";
            return false;
        }
        if (hasSsid) {
            if (ssid.empty()) {
                commands.push_back(simple(CommandType::WifiClear));
            } else {
                Command command = simple(CommandType::Wifi);
                command.text = ssid;
                command.text2 = password;
                command.flag = !hasPassword;  // keep the stored password
                commands.push_back(command);
            }
        }
        if (!optionalText(object, "apPassword", text, present, error)) return false;
        if (present) {
            if (!ConfigStore::isValidApPassword(text)) {
                error = "apPassword must be 8-63 printable ASCII characters";
                return false;
            }
            Command command = simple(CommandType::ApPassword);
            command.text = text;
            commands.push_back(command);
        }
        {
            Command command = simple(CommandType::Https);
            if (!optionalBool(object, "https", command.flag, present, error)) return false;
            if (present) commands.push_back(command);
        }
        bool save = false;
        if (!optionalBool(object, "save", save, present, error)) return false;
        if (save) commands.push_back(simple(CommandType::Save));
        if (commands.empty()) {
            error = "no settings given";
            return false;
        }
        return true;
    }

    error = "unknown endpoint";
    return false;
}

ApiResponse ApiExchange::submit(const std::string& endpoint, const std::string& body) {
    if (body.size() > kMaxBodySize) return {413, errorJson("request body too large")};
    Job job;
    std::string error;
    if (!parseApiRequest(endpoint, body, job.commands, error)) return {400, errorJson(error)};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job.id = nextId_++;
        if (nextId_ == 0) nextId_ = 1;
    }
    // Record the pending entry first: the loop may finish the job right after push().
    store(job.id, false, resultJson(job.id, false, {}));
    if (!queue_.push(job)) {
        store(job.id, true, resultJson(job.id, true, {false, "busy; request dropped"}));
        return {503, errorJson("busy; try again")};
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "{\"id\":%u}", static_cast<unsigned>(job.id));
    return {202, buffer};
}

ApiResponse ApiExchange::result(std::uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : results_) {
        if (entry.id == id && id != 0) return {entry.done ? 200 : 202, entry.json};
    }
    return {404, errorJson("unknown or expired request id")};
}

std::string ApiExchange::snapshot(Snapshot kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string& value = snapshots_[static_cast<std::size_t>(kind)];
    return value.empty() ? std::string("{}") : value;
}

void ApiExchange::publish(Snapshot kind, const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_[static_cast<std::size_t>(kind)] = json;
}

void ApiExchange::store(std::uint32_t id, bool done, const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : results_) {
        if (entry.id == id) {
            entry.done = done;
            entry.json = json;
            return;
        }
    }
    results_[resultHead_] = {id, done, json};
    resultHead_ = (resultHead_ + 1) % results_.size();
}

std::vector<std::string> ApiExchange::process(BridgeController& ui, std::size_t maxJobs) {
    std::vector<std::string> messages;
    Job job;
    for (std::size_t count = 0; count < maxJobs && queue_.pop(job); ++count) {
        CommandResult combined{true, ""};
        for (const auto& command : job.commands) {
            const CommandResult result = ui.execute(command);
            if (!combined.message.empty()) combined.message += "; ";
            combined.message += result.message;
            if (!result.ok) {
                combined.ok = false;
                break;  // later commands (e.g. save) must not run after a rejected one
            }
        }
        const std::string json = resultJson(job.id, true, combined);
        store(job.id, true, json);
        messages.push_back(json);
    }
    return messages;
}
