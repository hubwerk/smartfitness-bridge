#if defined(DEBUG_WEB)
#ifndef DEBUG_WEB_IDF_LEVEL
#define DEBUG_WEB_IDF_LEVEL 3
#endif
#if DEBUG_WEB_IDF_LEVEL < 1 || DEBUG_WEB_IDF_LEVEL > 5
#error "DEBUG_WEB_IDF_LEVEL must be between 1 (Error) and 5 (Verbose)"
#endif

#undef CORE_DEBUG_LEVEL
#define CORE_DEBUG_LEVEL DEBUG_WEB_IDF_LEVEL
#define USE_ESP_IDF_LOG
// Arduino-ESP32 2.0.x maps log_x() to ESP_LOGx(TAG, ...) under USE_ESP_IDF_LOG
// but leaves its default TAG commented out; framework libraries (LittleFS, ...)
// define none, so provide one for every translation unit.
#ifndef TAG
#define TAG "ARDUINO"
#endif

#undef CONFIG_NIMBLE_CPP_LOG_LEVEL
#define CONFIG_NIMBLE_CPP_LOG_LEVEL 2
#endif
