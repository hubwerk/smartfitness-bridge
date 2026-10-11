# HMI host simulator (included by the top-level Makefile).
# LVGL is compiled with the shared include/variants/hmi/lv_conf.h, the UI and
# the shared core with the same sources as the firmware.

SIM_CC ?= gcc
SIM_CXX ?= g++
SIM_FLAGS = -O1 -DLV_CONF_INCLUDE_SIMPLE -Iinclude/variants/hmi -I$(LVGL_DIR)
LVGL_SOURCES = $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
LVGL_OBJECTS = $(patsubst $(LVGL_DIR)/%.c,$(SIM_BUILD)/lvgl/%.o,$(LVGL_SOURCES))
SIM_SOURCES = sim/hmi_sim.cpp src/variants/hmi/hmi_ui.cpp src/variants/hmi/lcars_theme.cpp $(HMI_SOURCES) $(CORE_SOURCES)
FONT_SOURCES = $(wildcard src/variants/hmi/fonts/*.c)
FONT_OBJECTS = $(patsubst src/variants/hmi/fonts/%.c,$(SIM_BUILD)/fonts/%.o,$(FONT_SOURCES))
SIM_BINARY = $(SIM_BUILD)/hmi-sim

$(SIM_BUILD)/fonts/%.o: src/variants/hmi/fonts/%.c include/variants/hmi/lv_conf.h
	@mkdir -p $(dir $@)
	$(SIM_CC) $(SIM_FLAGS) -c $< -o $@

$(SIM_BUILD)/lvgl/%.o: $(LVGL_DIR)/%.c include/variants/hmi/lv_conf.h
	@mkdir -p $(dir $@)
	$(SIM_CC) $(SIM_FLAGS) -c $< -o $@

$(SIM_BINARY): $(LVGL_OBJECTS) $(FONT_OBJECTS) $(SIM_SOURCES) $(wildcard include/variants/hmi/*.h)
	@test -f $(LVGL_DIR)/lvgl.h || (echo "LVGL not found in $(LVGL_DIR) (see Makefile)" && exit 1)
	$(SIM_CXX) $(CXXFLAGS) $(SIM_FLAGS) -Iinclude/core $(SIM_SOURCES) $(LVGL_OBJECTS) $(FONT_OBJECTS) -o $@

hmi-sim: $(SIM_BINARY)

hmi-screenshots: $(SIM_BINARY)
	@mkdir -p $(SCREENSHOT_DIR)
	$(SIM_BINARY) $(SCREENSHOT_DIR)
