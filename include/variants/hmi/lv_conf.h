/**
 * LVGL 9.2 configuration shared by the HMI firmware (crowpanel128) and the
 * host simulator (sim/). Only deviations from the LVGL defaults are listed;
 * everything else comes from lv_conf_internal.h.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565, swapped to the panel byte order in the flush callback. */
#define LV_COLOR_DEPTH 16

/* C library allocator: on the ESP32-S3 with PSRAM enabled, large LVGL
 * allocations land in PSRAM, small ones stay in internal RAM. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* LVGL runs in the cooperative Arduino loop only. */
#define LV_USE_OS LV_OS_NONE

#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 200

#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/* ASCII Montserrat fonts. A German string table needs a font with Latin-1
 * glyphs (umlauts) generated with the LVGL font converter. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0

#endif /* LV_CONF_H */
