#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <cstdarg>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace fake {
inline bool isr = false;
inline unsigned serialCalls = 0;
inline unsigned fsCalls = 0;
inline unsigned mutexCalls = 0;
inline unsigned formats = 0;
inline unsigned opens = 0;
inline unsigned flushes = 0;
inline unsigned mounts = 0;
inline unsigned mutexAllocations = 0;
inline unsigned mutexAllocationFailAt = 0;
inline bool allMutexAllocationsFail = false;
inline bool taskAllocationFails = false;
inline bool mounted = true;
inline bool openFails = false;
inline bool readFails = false;
inline std::size_t writeLimit = SIZE_MAX;
inline std::size_t otherFiles = 0;
inline std::string file;
inline std::vector<std::uint8_t> partition(0x360000, 0xff);
inline std::size_t maxSize = 0;
inline int workerIterations = 0;
struct StopWorker {};
inline void (*task)(void*) = nullptr;
inline void* currentTask = nullptr;
}  // namespace fake

class Print {
public:
    virtual ~Print() = default;
    virtual std::size_t write(std::uint8_t byte) = 0;
    virtual std::size_t write(const std::uint8_t* data, std::size_t size) {
        std::size_t count = 0;
        while (size--) count += write(*data++);
        return count;
    }
    std::size_t write(const char* data) {
        return write(reinterpret_cast<const std::uint8_t*>(data), std::strlen(data));
    }
    std::size_t print(const char* text) { return write(text); }
    std::size_t println(const char* text) { return print(text) + write("\r\n"); }
};

class FakeSerial : public Print {
public:
    using Print::write;
    std::string output;
    std::size_t write(std::uint8_t byte) override { return write(&byte, 1); }
    std::size_t write(const std::uint8_t* data, std::size_t size) override {
        assert(!fake::isr);
        ++fake::serialCalls;
        output.append(reinterpret_cast<const char*>(data), size);
        return size;
    }
};
inline FakeSerial Serial;

using TickType_t = unsigned;
constexpr TickType_t portMAX_DELAY = ~TickType_t(0);
constexpr int pdTRUE = 1;
constexpr int pdPASS = 1;
using SemaphoreHandle_t = std::mutex*;
using TaskHandle_t = void*;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define pdMS_TO_TICKS(value) (value)
#define portENTER_CRITICAL(mutex) (mutex)->lock()
#define portEXIT_CRITICAL(mutex) (mutex)->unlock()
#define portENTER_CRITICAL_ISR(mutex) (mutex)->lock()
#define portEXIT_CRITICAL_ISR(mutex) (mutex)->unlock()
inline bool xPortInIsrContext() { return fake::isr; }
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return fake::currentTask; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    ++fake::mutexAllocations;
    if (fake::allMutexAllocationsFail || fake::mutexAllocations == fake::mutexAllocationFailAt) return nullptr;
    return new std::mutex;
}
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks) {
    assert(!fake::isr);
    ++fake::mutexCalls;
    if (ticks == 0) return mutex->try_lock();
    mutex->lock();
    return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
inline int xTaskCreate(void (*task)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t* handle) {
    if (fake::taskAllocationFails) return 0;
    fake::task = task;
    *handle = reinterpret_cast<void*>(1);
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) { assert(!fake::isr); }
inline unsigned ulTaskNotifyTake(int, TickType_t) {
    if (fake::workerIterations-- <= 0) throw fake::StopWorker{};
    return 1;
}
inline void vTaskDelay(TickType_t) {}

class File {
public:
    File() = default;
    explicit File(bool writable) : valid_(true), writable_(writable) {}
    explicit operator bool() const { return valid_; }
    void close() { valid_ = false; }
    void flush() { assert(!fake::isr); ++fake::flushes; }
    bool seek(std::uint32_t position) {
        position_ = position;
        return valid_ && position <= fake::file.size();
    }
    std::size_t write(const std::uint8_t* data, std::size_t size) {
        assert(!fake::isr && valid_ && writable_);
        ++fake::fsCalls;
        size = std::min(size, fake::writeLimit);
        if (position_ + size > fake::file.size()) fake::file.resize(position_ + size);
        std::memcpy(fake::file.data() + position_, data, size);
        position_ += size;
        fake::maxSize = std::max(fake::maxSize, fake::file.size());
        return size;
    }
    std::size_t read(std::uint8_t* data, std::size_t size) {
        assert(!fake::isr && valid_ && !writable_);
        ++fake::fsCalls;
        if (fake::readFails) return 0;
        size = std::min(size, fake::file.size() - position_);
        std::memcpy(data, fake::file.data() + position_, size);
        position_ += size;
        return size;
    }
private:
    bool valid_ = false;
    bool writable_ = false;
    std::size_t position_ = 0;
};

struct FakeLittleFS {
    bool begin(bool format) {
        assert(!fake::isr);
        ++fake::mounts;
        if (!fake::mounted && format) {
            ++fake::formats;
            fake::mounted = true;
        }
        return fake::mounted;
    }
    File open(const char* path, const char* mode) {
        assert(!fake::isr && std::strcmp(path, "/debug.log") == 0);
        ++fake::opens;
        if (fake::openFails) return {};
        if (*mode == 'w') fake::file.clear();
        return File(*mode == 'w');
    }
    std::size_t totalBytes() const { return fake::partition.size(); }
    std::size_t usedBytes() const { return fake::file.size() + fake::otherFiles; }
};
inline FakeLittleFS LittleFS;

constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 130;
constexpr int ESP_OK = 0;
struct esp_partition_t { std::size_t size; };
inline const esp_partition_t* esp_partition_find_first(int, int, const char*) {
    static esp_partition_t partition{fake::partition.size()};
    return &partition;
}
inline int esp_partition_read(const esp_partition_t*, std::size_t offset, void* data, std::size_t count) {
    assert(!fake::isr);
    std::memcpy(data, fake::partition.data() + offset, count);
    return ESP_OK;
}
