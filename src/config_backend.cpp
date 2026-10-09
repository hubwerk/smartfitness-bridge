#include "config_backend.h"
#ifdef ARDUINO
#include <Preferences.h>
#endif

bool MemoryConfigBackend::read(std::vector<std::uint8_t>& data) {
    if (!hasRecord_) return false;
    data = record_;
    return true;
}

bool MemoryConfigBackend::write(const std::vector<std::uint8_t>& data) {
    if (failWrites || data.size() > kMaxRecordSize) return false;
    record_ = data;
    hasRecord_ = true;
    ++writes_;
    return true;
}

bool MemoryConfigBackend::erase() {
    record_.clear();
    hasRecord_ = false;
    ++erases_;
    return true;
}

void MemoryConfigBackend::setRecord(const std::vector<std::uint8_t>& data) {
    record_ = data;
    hasRecord_ = true;
}

#ifdef ARDUINO
bool NvsConfigBackend::read(std::vector<std::uint8_t>& data) {
    data.clear();
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return false;
    if (!preferences.isKey(kKey)) {
        preferences.end();
        return false;
    }
    const size_t length = preferences.getBytesLength(kKey);
    // An oversized record is reported as present but empty so it is treated as corrupt.
    if (length > 0 && length <= kMaxRecordSize) {
        data.resize(length);
        if (preferences.getBytes(kKey, data.data(), length) != length) data.clear();
    }
    preferences.end();
    return true;
}

bool NvsConfigBackend::write(const std::vector<std::uint8_t>& data) {
    if (data.empty() || data.size() > kMaxRecordSize) return false;
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return false;
    const bool written = preferences.putBytes(kKey, data.data(), data.size()) == data.size();
    preferences.end();
    return written;
}

bool NvsConfigBackend::erase() {
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return false;
    const bool cleared = preferences.clear();
    preferences.end();
    return cleared;
}
#endif
