#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Storage for the serialized configuration record. Implementations store one
// opaque blob; validation and versioning are handled by ConfigStore.
class ConfigBackend {
public:
    static constexpr std::size_t kMaxRecordSize = 512;

    virtual ~ConfigBackend() = default;
    // Returns false when no record exists (or it cannot be read).
    virtual bool read(std::vector<std::uint8_t>& data) = 0;
    virtual bool write(const std::vector<std::uint8_t>& data) = 0;
    // Removes all stored configuration.
    virtual bool erase() = 0;
};

// RAM-only backend for host tests; survives across ConfigStore instances
// that share it, not across reboot.
class MemoryConfigBackend final : public ConfigBackend {
public:
    bool read(std::vector<std::uint8_t>& data) override;
    bool write(const std::vector<std::uint8_t>& data) override;
    bool erase() override;

    bool hasRecord() const { return hasRecord_; }
    const std::vector<std::uint8_t>& record() const { return record_; }
    void setRecord(const std::vector<std::uint8_t>& data);
    unsigned int writes() const { return writes_; }
    unsigned int erases() const { return erases_; }
    bool failWrites = false;

private:
    std::vector<std::uint8_t> record_;
    bool hasRecord_ = false;
    unsigned int writes_ = 0;
    unsigned int erases_ = 0;
};

#ifdef ARDUINO
// Flash (NVS) backend using the Arduino-ESP32 Preferences API.
class NvsConfigBackend final : public ConfigBackend {
public:
    static constexpr const char* kNamespace = "sfbridge";
    static constexpr const char* kKey = "config";

    bool read(std::vector<std::uint8_t>& data) override;
    bool write(const std::vector<std::uint8_t>& data) override;
    bool erase() override;
};
#endif
