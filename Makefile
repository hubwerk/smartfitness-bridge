CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -pthread

# Shared core: BLE input, protocols, BridgeCore, FTMS output, configuration,
# command layer, BridgeController + view model.
CORE_SOURCES = $(sort $(wildcard src/core/*.cpp src/core/protocols/*.cpp))
CORE_TESTS = tests/host/test_main.cpp tests/host/test_bridge_core.cpp tests/host/test_controller.cpp \
	tests/host/test_ftms.cpp tests/host/test_config_store.cpp tests/host/test_ble_input.cpp \
	tests/host/test_ifit_bowflex.cpp tests/host/test_trainers_rower.cpp tests/host/test_treadmills.cpp

# Web variant (pure C++ parts): API exchange, JSON renderer, Wi-Fi policy.
WEB_SOURCES = src/variants/web/web_api.cpp src/variants/web/web_json.cpp src/variants/web/wifi_policy.cpp
WEB_TESTS = tests/host/test_web.cpp

# HMI variant (pure C++ parts): view logic shared by the LVGL screens.
HMI_SOURCES = src/variants/hmi/hmi_model.cpp src/variants/hmi/hmi_strings.cpp
HMI_TESTS = tests/host/test_hmi_model.cpp

CORE_TEST_BINARY = /tmp/smartfitness-core-only-test
HOST_TEST_BINARY = /tmp/smartfitness-core-test

.PHONY: host-tests host-tests-core host-tests-all hmi-sim hmi-screenshots

host-tests: host-tests-core host-tests-all

# Core only: only include/core on the include path and no variant source, so
# this proves the core compiles and links without web or HMI code.
host-tests-core:
	$(CXX) $(CXXFLAGS) -Iinclude/core -Itests/host $(CORE_TESTS) $(CORE_SOURCES) -o $(CORE_TEST_BINARY)
	$(CORE_TEST_BINARY)

host-tests-all:
	$(CXX) $(CXXFLAGS) -DSF_TEST_WEB -DSF_TEST_HMI -Iinclude/core -Iinclude/variants/web -Iinclude/variants/hmi \
		-Itests/host -Itests/host/third_party $(CORE_TESTS) $(WEB_TESTS) $(HMI_TESTS) \
		$(CORE_SOURCES) $(WEB_SOURCES) $(HMI_SOURCES) -o $(HOST_TEST_BINARY)
	$(HOST_TEST_BINARY)

# LVGL host simulator: renders every HMI screen from fake view-model data
# into PNG files (no display needed). Requires an LVGL v9.2.2 checkout:
#   git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git $(LVGL_DIR)
LVGL_DIR ?= .cache/lvgl
SIM_BUILD ?= /tmp/smartfitness-hmi-sim
SCREENSHOT_DIR ?= screenshots
include sim/hmi_sim.mk
