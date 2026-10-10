#include "config_store.h"
#include "ftms_output.h"

static_assert(ConfigStore::kMaxFtmsNameLength == FtmsOutput::kMaxNameLength,
              "persisted FTMS name limit must match the output");

namespace {
// Record layout (schema version 3):
//   'S' 'F' version antEnabled ftmsInput ftmsOutput simulatedTrainer
//   ftmsNameLength ftmsName[...] pairedNameLength pairedName[...]
//   adapterIdLength adapterId[...] ssidLength ssid[...]
//   wifiPasswordLength wifiPassword[...] apPasswordLength apPassword[...]
//   sourceValid sourceAddressType sourceAddress[6] ftmsOutputProfile crc32 (little endian)
// Version 1 records lack the four v2 text fields; v1/v2 lack the profile byte.
// Missing fields take their defaults on load. Worst case size is 252 bytes.
constexpr std::uint8_t kMagic0 = 'S';
constexpr std::uint8_t kMagic1 = 'F';
// Header (3) + booleans (4) + two empty name lengths (2) + paired source (8) + CRC (4).
// (v1 minimum; v2 adds four length bytes and a non-empty AP password.)
constexpr std::size_t kMinRecordSize = 21;

std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

bool isPrintableAscii(const std::string& value) {
    for (const char character : value) {
        if (character < 0x20 || character > 0x7E) return false;
    }
    return true;
}

class Reader {
public:
    Reader(const std::vector<std::uint8_t>& data, std::size_t end) : data_(data), end_(end) {}
    bool readByte(std::uint8_t& value) {
        if (offset_ >= end_) return false;
        value = data_[offset_++];
        return true;
    }
    bool readBool(bool& value) {
        std::uint8_t raw = 0;
        if (!readByte(raw) || raw > 1) return false;
        value = raw == 1;
        return true;
    }
    bool readText(std::string& value, std::size_t maxLength) {
        std::uint8_t length = 0;
        if (!readByte(length) || length > maxLength || end_ - offset_ < length) return false;
        value.assign(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                     data_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
        offset_ += length;
        return true;
    }
    bool done() const { return offset_ == end_; }

private:
    const std::vector<std::uint8_t>& data_;
    std::size_t end_;
    std::size_t offset_ = 0;
};
}  // namespace

ConfigStore::ConfigStore() : ownedBackend_(new MemoryConfigBackend), backend_(*ownedBackend_) {}

ConfigStore::ConfigStore(ConfigBackend& backend) : backend_(backend) {}

ConfigLoadResult ConfigStore::begin() {
    return load();
}

ConfigLoadResult ConfigStore::load() {
    std::vector<std::uint8_t> data;
    ConfigLoadResult result = ConfigLoadResult::Missing;
    BridgeConfig loaded;
    if (!backend_.read(data)) {
        persisted_ = serialize(loaded);  // no record is equivalent to defaults
    } else if (deserialize(data, loaded)) {
        result = ConfigLoadResult::Loaded;
        persisted_ = data;
    } else {
        result = ConfigLoadResult::Invalid;
        loaded = BridgeConfig{};
        persisted_ = data;  // differs from any valid record, so the next save rewrites it
    }
    saved_ = loaded;
    config_ = saved_;
    return result;
}

ConfigSaveResult ConfigStore::save() {
    if (!isValid(config_)) return ConfigSaveResult::Invalid;
    auto data = serialize(config_);
    if (data == persisted_) {
        saved_ = config_;
        return ConfigSaveResult::Unchanged;
    }
    if (!backend_.write(data)) return ConfigSaveResult::Failed;
    persisted_ = std::move(data);
    saved_ = config_;
    return ConfigSaveResult::Written;
}

bool ConfigStore::pairSource(const PairedSourceDevice& source, const std::string& name,
                             const std::string& adapterId) {
    if (!source.valid || source.addressType > kMaxAddressType || !isValidAdapterId(adapterId) || adapterId.empty())
        return false;
    const std::string sanitized = sanitizeName(name);
    BridgeConfig next = saved_;
    next.pairedSource = source;
    next.pairedDeviceName = sanitized;
    next.pairedAdapterId = adapterId;
    next.ftms.input = true;
    if (!persistPartial(next)) return false;
    config_.pairedSource = source;
    config_.pairedDeviceName = sanitized;
    config_.pairedAdapterId = adapterId;
    config_.ftms.input = true;
    return true;
}

bool ConfigStore::unpair() {
    config_.pairedDeviceName.clear();
    config_.pairedSource = {};
    config_.pairedAdapterId.clear();
    BridgeConfig next = saved_;
    next.pairedDeviceName.clear();
    next.pairedSource = {};
    next.pairedAdapterId.clear();
    return persistPartial(next);
}

bool ConfigStore::persistPartial(const BridgeConfig& next) {
    auto data = serialize(next);
    if (data == persisted_) {
        saved_ = next;
        return true;
    }
    if (!backend_.write(data)) return false;
    persisted_ = std::move(data);
    saved_ = next;
    return true;
}

bool ConfigStore::factoryReset() {
    const bool erased = backend_.erase();
    config_ = BridgeConfig{};
    if (erased) {
        saved_ = config_;
        persisted_ = serialize(config_);
    }
    return erased;
}

bool ConfigStore::hasUnsavedChanges() const {
    return config_ != saved_;
}

bool ConfigStore::hasPairedDevice() const {
    return !config_.pairedDeviceName.empty() || config_.pairedSource.valid;
}

const std::string& ConfigStore::pairedDeviceName() const {
    return config_.pairedDeviceName;
}

BridgeConfig& ConfigStore::config() { return config_; }
const BridgeConfig& ConfigStore::config() const { return config_; }

std::string ConfigStore::sanitizeName(const std::string& name) {
    std::string sanitized = name.substr(0, kMaxPairedNameLength);
    for (char& character : sanitized) {
        if (character < 0x20 || character > 0x7E) character = '?';
    }
    return sanitized;
}

void ConfigStore::pairDevice(const std::string& name) {
    config_.pairedDeviceName = sanitizeName(name);
}

bool ConfigStore::isValidFtmsName(const std::string& name) {
    return name.size() <= kMaxFtmsNameLength && isPrintableAscii(name);
}

bool ConfigStore::isValidAdapterId(const std::string& id) {
    if (id.size() > kMaxAdapterIdLength) return false;
    for (const char character : id) {
        const bool ok = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                        character == '-' || character == '_';
        if (!ok) return false;
    }
    return true;
}

bool ConfigStore::isValidSsid(const std::string& ssid) {
    if (ssid.size() > kMaxSsidLength) return false;
    for (const char character : ssid) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7F) return false;  // UTF-8 bytes are allowed, control characters are not
    }
    return true;
}

bool ConfigStore::isValidPassword(const std::string& password, bool allowEmpty) {
    if (password.empty()) return allowEmpty;
    return password.size() >= kMinPasswordLength && password.size() <= kMaxPasswordLength &&
           isPrintableAscii(password);
}

bool ConfigStore::isValidApPassword(const std::string& password) {
    return isValidPassword(password, false);
}

bool ConfigStore::isValid(const BridgeConfig& config) {
    if (static_cast<std::uint8_t>(config.ftmsOutputProfile) >
        static_cast<std::uint8_t>(smartfitness::FtmsOutputProfile::Rower)) return false;
    if (!isValidFtmsName(config.ftmsName)) return false;
    if (!isValidAdapterId(config.pairedAdapterId) || !isValidSsid(config.wifiSsid) ||
        !isValidPassword(config.wifiPassword, true) || !isValidApPassword(config.apPassword))
        return false;
    if (config.pairedDeviceName.size() > kMaxPairedNameLength || !isPrintableAscii(config.pairedDeviceName))
        return false;
    const auto& source = config.pairedSource;
    if (source.addressType > kMaxAddressType) return false;
    if (!source.valid && source != PairedSourceDevice{}) return false;
    return true;
}

std::vector<std::uint8_t> ConfigStore::serialize(const BridgeConfig& config) {
    std::vector<std::uint8_t> data{kMagic0, kMagic1, kSchemaVersion,
                                   static_cast<std::uint8_t>(config.antEnabled),
                                   static_cast<std::uint8_t>(config.ftms.input),
                                   static_cast<std::uint8_t>(config.ftms.output),
                                   static_cast<std::uint8_t>(config.simulatedTrainer)};
    const auto appendText = [&data](const std::string& value) {
        data.push_back(static_cast<std::uint8_t>(value.size()));
        data.insert(data.end(), value.begin(), value.end());
    };
    appendText(config.ftmsName);
    appendText(config.pairedDeviceName);
    appendText(config.pairedAdapterId);
    appendText(config.wifiSsid);
    appendText(config.wifiPassword);
    appendText(config.apPassword);
    data.push_back(static_cast<std::uint8_t>(config.pairedSource.valid));
    data.push_back(config.pairedSource.addressType);
    data.insert(data.end(), config.pairedSource.address.begin(), config.pairedSource.address.end());
    data.push_back(static_cast<std::uint8_t>(config.ftmsOutputProfile));
    const std::uint32_t crc = crc32(data.data(), data.size());
    for (int shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::uint8_t>(crc >> shift));
    return data;
}

bool ConfigStore::deserialize(const std::vector<std::uint8_t>& data, BridgeConfig& config) {
    if (data.size() < kMinRecordSize || data.size() > ConfigBackend::kMaxRecordSize) return false;
    const std::size_t end = data.size() - 4;
    std::uint32_t stored = 0;
    for (int index = 3; index >= 0; --index) stored = (stored << 8) | data[end + static_cast<std::size_t>(index)];
    if (stored != crc32(data.data(), end)) return false;

    Reader reader(data, end);
    std::uint8_t magic0 = 0, magic1 = 0, version = 0;
    if (!reader.readByte(magic0) || !reader.readByte(magic1) || magic0 != kMagic0 || magic1 != kMagic1) return false;
    // Older records are migrated; unknown versions fall back to defaults.
    if (!reader.readByte(version) || version < 1 || version > kSchemaVersion) return false;

    BridgeConfig next;
    std::uint8_t sourceValid = 0;
    if (!reader.readBool(next.antEnabled) || !reader.readBool(next.ftms.input) ||
        !reader.readBool(next.ftms.output) || !reader.readBool(next.simulatedTrainer) ||
        !reader.readText(next.ftmsName, kMaxFtmsNameLength) ||
        !reader.readText(next.pairedDeviceName, kMaxPairedNameLength))
        return false;
    if (version >= 2 &&
        (!reader.readText(next.pairedAdapterId, kMaxAdapterIdLength) ||
         !reader.readText(next.wifiSsid, kMaxSsidLength) ||
         !reader.readText(next.wifiPassword, kMaxPasswordLength) ||
         !reader.readText(next.apPassword, 255)))
        return false;
    if (!reader.readByte(sourceValid) ||
        sourceValid > 1 || !reader.readByte(next.pairedSource.addressType))
        return false;
    next.pairedSource.valid = sourceValid == 1;
    for (auto& octet : next.pairedSource.address) {
        if (!reader.readByte(octet)) return false;
    }
    if (version >= 3) {
        std::uint8_t profile = 0;
        if (!reader.readByte(profile)) return false;
        next.ftmsOutputProfile = static_cast<smartfitness::FtmsOutputProfile>(profile);
    }
    if (!reader.done()) return false;
    // Recover a legacy invalid AP passphrase without losing the paired source
    // or station credentials. The original record is retained until save().
    if (!isValidApPassword(next.apPassword)) next.apPassword = BridgeConfig{}.apPassword;
    if (!isValid(next)) return false;
    config = next;
    return true;
}
