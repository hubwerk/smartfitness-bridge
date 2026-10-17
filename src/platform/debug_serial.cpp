#if defined(DEBUG_WEB) && defined(ARDUINO)
#include "debug_serial.h"

#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef DEBUG_WEB_IDF_LEVEL
#define DEBUG_WEB_IDF_LEVEL 3
#endif

namespace {
constexpr char kPath[] = "/debug.log";
constexpr std::size_t kQueueBytes = 8192;
constexpr std::size_t kBlockBytes = 1024;
std::uint8_t queue[kQueueBytes];
std::uint8_t queueToSerial[kQueueBytes];
std::size_t head = 0;
std::size_t queued = 0;
std::uint64_t lost = 0;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t fsMutex = nullptr;
SemaphoreHandle_t serialMutex = nullptr;
TaskHandle_t workerTask = nullptr;
File logFile;
std::uint64_t absoluteStart = 0;
std::uint64_t absoluteEnd = 0;
bool writable = false;
bool started = false;
bool serialOnly = false;
vprintf_like_t uartVprintf = nullptr;
thread_local bool inDebugVprintf = false;

class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex, TickType_t timeout = portMAX_DELAY)
        : mutex_(mutex), locked_(mutex && xSemaphoreTake(mutex, timeout) == pdTRUE) {}
    ~Lock() { if (locked_) xSemaphoreGive(mutex_); }
    explicit operator bool() const { return locked_; }
private:
    SemaphoreHandle_t mutex_;
    bool locked_;
};

void serialWrite(const void* data, std::size_t size) {
    Lock lock(serialMutex);
    if (!serialMutex || lock) Serial.write(static_cast<const std::uint8_t*>(data), size);
}

void warning(const char* message) {
    serialWrite(message, std::strlen(message));
}

// A failed mount is never formatted unless the ENTIRE existing filesystem
// partition is erased. In particular, a damaged or foreign filesystem is kept.
bool blankPartition() {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!partition) return false;
    std::uint8_t block[kBlockBytes];
    for (std::size_t offset = 0; offset < partition->size; offset += sizeof(block)) {
        const std::size_t count = std::min(sizeof(block), partition->size - offset);
        if (esp_partition_read(partition, offset, block, count) != ESP_OK) return false;
        for (std::size_t i = 0; i < count; ++i) {
            if (block[i] != 0xff) return false;
        }
    }
    return true;
}

bool rejectIsr() {
    if (!xPortInIsrContext()) return false;
    portENTER_CRITICAL_ISR(&queueMux);
    ++lost;
    portEXIT_CRITICAL_ISR(&queueMux);
    return true;
}

void recordLoss(std::size_t size) {
    portENTER_CRITICAL(&queueMux);
    lost += size;
    portEXIT_CRITICAL(&queueMux);
}

std::size_t lossMarker(char (&marker)[100], std::uint64_t dropped) {
    return std::snprintf(marker, sizeof(marker),
        "\n[Debug] LOST OUTPUT: %llu byte(s)/ISR call(s)\n",
        static_cast<unsigned long long>(dropped));
}

std::size_t stripAnsi(char* text, std::size_t size) {
    std::size_t read = 0;
    std::size_t written = 0;
    while (read < size) {
        if (static_cast<unsigned char>(text[read]) == 0x1b && read + 1 < size && text[read + 1] == '[') {
            read += 2;
            while (read < size) {
                const unsigned char character = static_cast<unsigned char>(text[read++]);
                if (character >= 0x40 && character <= 0x7e) break;
            }
            continue;
        }
        text[written++] = text[read++];
    }
    return written;
}

int debugVprintf(const char* format, va_list args) {
    if (!format || inDebugVprintf) return 0;
    inDebugVprintf = true;

    va_list uartArgs;
    va_copy(uartArgs, args);
    const int uartResult = uartVprintf ? uartVprintf(format, uartArgs) : 0;
    va_end(uartArgs);

    if (workerTask && xTaskGetCurrentTaskHandle() == workerTask) {
        inDebugVprintf = false;
        return uartResult;
    }

    char buffer[kBlockBytes / 2];
    va_list logArgs;
    va_copy(logArgs, args);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, logArgs);
    va_end(logArgs);
    if (length > 0) {
        const std::size_t available = std::min(static_cast<std::size_t>(length), sizeof(buffer) - 1);
        if (static_cast<std::size_t>(length) > available) recordLoss(static_cast<std::size_t>(length) - available);
        const std::size_t clean = stripAnsi(buffer, available);
        debugSerial.writeLog(reinterpret_cast<const std::uint8_t*>(buffer), clean);
    }
    inDebugVprintf = false;
    return uartResult;
}

void useSerialFallback(const char* reason) {
    serialOnly = true;
    workerTask = nullptr;
    writable = false;
    logFile.close();
    // If only the filesystem mutex was allocated, reuse it to serialize output.
    if (!serialMutex) {
        serialMutex = fsMutex;
        fsMutex = nullptr;
    }
    warning(reason);
}

std::size_t fallbackWrite(const std::uint8_t* data, std::size_t size) {
    Lock lock(serialMutex);
    if (serialMutex && !lock) return 0;
    portENTER_CRITICAL(&queueMux);
    const std::uint64_t dropped = lost + queued;
    lost = 0;
    queued = 0;
    portEXIT_CRITICAL(&queueMux);
    if (dropped) {
        char marker[100];
        const std::size_t count = lossMarker(marker, dropped);
        Serial.write(reinterpret_cast<const std::uint8_t*>(marker), count);
    }
    return Serial.write(data, size);
}

void append(const std::uint8_t* data, std::size_t size) {
    Lock lock(fsMutex);
    if (!lock || !writable) return;
    // Whole-file rotation bounds flash usage, keeping absolute cursors monotonic.
    if (absoluteEnd - absoluteStart + size > debugweb::kMaxLogBytes) {
        logFile.close();
        logFile = LittleFS.open(kPath, "w");
        absoluteStart = absoluteEnd;
        if (!logFile) {
            writable = false;
            warning("[Debug] log rotation failed; file logging disabled\n");
            return;
        }
    }
    const std::size_t written = logFile.write(data, size);
    absoluteEnd += written;
    if (written != size) {
        writable = false;
        warning("[Debug] log write failed; persistent output lost\n");
    }
}

void worker(void*) {
    std::uint8_t block[kBlockBytes];
    for (;;) {
        // Timeout also surfaces ISR losses: ISR producers never call RTOS
        // notification APIs, Serial, filesystem code, or any blocking function.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        portENTER_CRITICAL(&queueMux);
        std::size_t remaining = queued;
        const std::uint64_t dropped = lost;
        lost = 0;
        portEXIT_CRITICAL(&queueMux);
        if (remaining == 0 && dropped == 0) continue;
        vTaskDelay(pdMS_TO_TICKS(20));
        while (remaining) {
            portENTER_CRITICAL(&queueMux);
            std::size_t count = std::min(remaining, sizeof(block));
            const bool toSerial = queueToSerial[head] != 0;
            for (std::size_t index = 1; index < count; ++index) {
                if ((queueToSerial[(head + index) % kQueueBytes] != 0) != toSerial) {
                    count = index;
                    break;
                }
            }
            const std::size_t first = std::min(count, kQueueBytes - head);
            std::memcpy(block, queue + head, first);
            std::memcpy(block + first, queue, count - first);
            head = (head + count) % kQueueBytes;
            queued -= count;
            portEXIT_CRITICAL(&queueMux);
            if (toSerial) serialWrite(block, count);
            append(block, count);
            remaining -= count;
        }
        if (dropped) {
            char marker[100];
            const std::size_t count = lossMarker(marker, dropped);
            serialWrite(marker, count);
            append(reinterpret_cast<const std::uint8_t*>(marker), count);
        }
        Lock lock(fsMutex);
        if (lock && logFile) logFile.flush();
    }
}
}  // namespace

DebugSerial debugSerial;

std::size_t DebugSerial::write(std::uint8_t byte) {
    return write(&byte, 1);
}

std::size_t DebugSerial::write(const std::uint8_t* data, std::size_t size) {
    if (rejectIsr()) return 0;
    if (!data || !size) return 0;
    if (serialOnly) return fallbackWrite(data, size);
    portENTER_CRITICAL(&queueMux);
    const std::size_t count = std::min(size, kQueueBytes - queued);
    const std::size_t tail = (head + queued) % kQueueBytes;
    const std::size_t first = std::min(count, kQueueBytes - tail);
    std::memcpy(queue + tail, data, first);
    std::memcpy(queue, data + first, count - first);
    std::memset(queueToSerial + tail, 1, first);
    std::memset(queueToSerial, 1, count - first);
    queued += count;
    lost += size - count;
    portEXIT_CRITICAL(&queueMux);
    if (workerTask) xTaskNotifyGive(workerTask);
    return count;
}

std::size_t DebugSerial::writeLog(const std::uint8_t* data, std::size_t size) {
    if (rejectIsr()) return 0;
    if (!data || !size) return 0;
    if (serialOnly) return size;
    portENTER_CRITICAL(&queueMux);
    const std::size_t count = std::min(size, kQueueBytes - queued);
    const std::size_t tail = (head + queued) % kQueueBytes;
    const std::size_t first = std::min(count, kQueueBytes - tail);
    std::memcpy(queue + tail, data, first);
    std::memcpy(queue, data + first, count - first);
    std::memset(queueToSerial + tail, 0, first);
    std::memset(queueToSerial, 0, count - first);
    queued += count;
    lost += size - count;
    portEXIT_CRITICAL(&queueMux);
    if (workerTask) xTaskNotifyGive(workerTask);
    return count;
}

std::size_t DebugSerial::printf(const char* format, ...) {
    if (rejectIsr() || !format) return 0;
    char buffer[kBlockBytes];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length <= 0) return 0;
    const std::size_t count = std::min(static_cast<std::size_t>(length), sizeof(buffer) - 1);
    if (static_cast<std::size_t>(length) > count) recordLoss(length - count);
    return write(reinterpret_cast<const std::uint8_t*>(buffer), count);
}

namespace debugweb {
void begin() {
    if (rejectIsr() || started) return;
    started = true;
    fsMutex = xSemaphoreCreateMutex();
    serialMutex = xSemaphoreCreateMutex();
    if (!fsMutex || !serialMutex) {
        useSerialFallback("[Debug] logger mutex allocation failed; direct Serial fallback\n");
        return;
    }
    bool mounted = LittleFS.begin(false);
    if (!mounted && blankPartition()) mounted = LittleFS.begin(true);
    if (!mounted) {
        warning("[Debug] LittleFS mount failed; existing partition NOT formatted; Serial only\n");
    } else {
        // Truncate only AFTER a successful mount, once per boot.
        logFile = LittleFS.open(kPath, "w");
        writable = static_cast<bool>(logFile);
        if (!writable) warning("[Debug] cannot open /debug.log; Serial only\n");
        if (LittleFS.totalBytes() - LittleFS.usedBytes() < kMaxLogBytes) {
            warning("[Debug] insufficient free LittleFS space for 512 KiB log\n");
        }
    }
    if (xTaskCreate(worker, "debug-log", 4096, nullptr, 1, &workerTask) != pdPASS) {
        useSerialFallback("[Debug] logger worker allocation failed; direct Serial fallback\n");
    }
    uartVprintf = esp_log_set_vprintf(debugVprintf);
    esp_log_level_set("*", static_cast<esp_log_level_t>(DEBUG_WEB_IDF_LEVEL));
}

Chunk readChunk(std::uint64_t offset, char* data, std::size_t capacity) {
    Chunk result;
    if (rejectIsr()) return result;
    Lock lock(fsMutex, 0);
    if (!lock || !logFile) return result;
    result.reset = offset < absoluteStart;
    result.start = std::min(std::max(offset, absoluteStart), absoluteEnd);
    result.end = result.start;
    if (!data || !capacity || result.start == absoluteEnd) return result;
    logFile.flush();
    File reader = LittleFS.open(kPath, "r");
    if (!reader || !reader.seek(static_cast<std::uint32_t>(result.start - absoluteStart))) return result;
    result.size = reader.read(reinterpret_cast<std::uint8_t*>(data),
        std::min<std::uint64_t>(capacity, absoluteEnd - result.start));
    result.end += result.size;
    return result;
}

bool streamLog(LogSink sink, void* context) {
    if (rejectIsr() || !sink) return false;
    Lock lock(fsMutex);
    if (!lock || !logFile) return false;
    logFile.flush();
    File reader = LittleFS.open(kPath, "r");
    if (!reader) return false;
    const std::uint64_t snapshotEnd = absoluteEnd;
    if (!sink(nullptr, 0, snapshotEnd, context)) return false;
    char block[kBlockBytes];
    std::uint64_t remaining = snapshotEnd - absoluteStart;
    while (remaining) {
        const std::size_t count = reader.read(reinterpret_cast<std::uint8_t*>(block),
            std::min<std::uint64_t>(remaining, sizeof(block)));
        if (!count || !sink(block, count, snapshotEnd, context)) return false;
        remaining -= count;
    }
    return true;
}
}  // namespace debugweb
#endif
