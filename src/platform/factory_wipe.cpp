#ifdef ARDUINO
#include "platform/factory_wipe.h"

#include <Arduino.h>

#include <esp_err.h>
#include <esp_partition.h>
#include <nvs_flash.h>

#include "debug_serial.h"

namespace platform {
namespace {
void erasePartition(esp_partition_subtype_t subtype, const char* label) {
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, nullptr);
    if (!partition) return;
    const esp_err_t result = esp_partition_erase_range(partition, 0, partition->size);
    DBG_SERIAL.printf("[Reset] %s partition (%u KiB) %s\n", label, static_cast<unsigned>(partition->size / 1024),
                      result == ESP_OK ? "erased" : esp_err_to_name(result));
}
}  // namespace

void wipeAllStorage() {
    DBG_SERIAL.println("[Reset] erasing all persistent storage");
#ifdef DEBUG_WEB
    debugweb::shutdown();
#endif
    // De-initializes NVS first; open handles (Wi-Fi, BLE) then fail instead of writing.
    const esp_err_t nvs = nvs_flash_erase();
    DBG_SERIAL.printf("[Reset] NVS %s\n", nvs == ESP_OK ? "erased" : esp_err_to_name(nvs));
    // A blank partition is formatted on the next boot, exactly like a freshly flashed device.
    erasePartition(ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "LittleFS");
    erasePartition(ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "core dump");
}
}  // namespace platform
#endif
