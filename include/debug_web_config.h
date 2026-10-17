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

#undef CONFIG_NIMBLE_CPP_LOG_LEVEL
#define CONFIG_NIMBLE_CPP_LOG_LEVEL 2
#endif
