CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -pthread

# Shared core: BLE input, protocols, BridgeCore, FTMS output, configuration,
# command layer, BridgeController + view model.
CORE_SOURCES = $(sort $(wildcard src/core/*.cpp src/core/protocols/*.cpp))
CORE_TESTS = tests/host/test_main.cpp tests/host/test_bridge_core.cpp tests/host/test_controller.cpp \
	tests/host/test_ftms.cpp tests/host/test_config_store.cpp tests/host/test_ble_input.cpp \
	tests/host/test_ifit_bowflex.cpp tests/host/test_trainers_rower.cpp tests/host/test_treadmills.cpp \
	tests/host/test_sportstech.cpp tests/host/test_fitshow.cpp tests/host/test_toorx.cpp tests/host/test_domyos_ext.cpp \
	tests/host/test_echelon_ext.cpp tests/host/test_nautilus.cpp tests/host/test_sportsplus.cpp \
	tests/host/test_vendor_treadmills.cpp tests/host/test_vendor_bikes.cpp tests/host/test_vendor_cardio.cpp \
	tests/host/test_ant.cpp

# Web variant (pure C++ parts): API exchange, JSON renderer, Wi-Fi policy.
WEB_SOURCES = src/variants/web/web_api.cpp src/variants/web/web_json.cpp src/variants/web/wifi_policy.cpp
WEB_TESTS = tests/host/test_web.cpp

# HMI variant (pure C++ parts): view logic shared by the LVGL screens.
HMI_SOURCES = src/variants/hmi/hmi_model.cpp src/variants/hmi/hmi_strings.cpp
HMI_TESTS = tests/host/test_hmi_model.cpp

CORE_TEST_BINARY = /tmp/smartfitness-core-only-test
HOST_TEST_BINARY = /tmp/smartfitness-core-test
DEBUG_TEST_BINARY = /tmp/smartfitness-debug-test

.PHONY: host-tests host-tests-core host-tests-all host-tests-debug host-tests-ftms-debug variant-guard hmi-sim hmi-screenshots

host-tests: host-tests-core host-tests-all host-tests-debug host-tests-ftms-debug variant-guard

# include/variant.h must accept exactly one of SF_VARIANT_WEB / SF_VARIANT_HMI.
variant-guard:
	@echo '#include "variant.h"' | $(CXX) $(CXXFLAGS) -DSF_VARIANT_WEB -Iinclude -x c++ -fsyntax-only -
	@echo '#include "variant.h"' | $(CXX) $(CXXFLAGS) -DSF_VARIANT_HMI -Iinclude -x c++ -fsyntax-only -
	@! echo '#include "variant.h"' | $(CXX) $(CXXFLAGS) -Iinclude -x c++ -fsyntax-only - 2>/dev/null
	@! echo '#include "variant.h"' | $(CXX) $(CXXFLAGS) -DSF_VARIANT_WEB -DSF_VARIANT_HMI -Iinclude -x c++ -fsyntax-only - 2>/dev/null
	@echo "variant guard passed"

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

host-tests-debug:
	$(CXX) $(CXXFLAGS) -DDEBUG_WEB -DARDUINO -Iinclude/core -Itests/host/debug_serial_fakes \
		-Itests/host tests/host/test_debug_serial.cpp tests/host/test_debug_ble.cpp \
		src/core/ble_input_adapter.cpp src/core/ble_types.cpp src/core/bt_device_adapter.cpp \
		src/core/bridge_core.cpp src/core/bridge_output.cpp \
		src/core/scan_label.cpp src/core/source_protocol.cpp src/core/ftms_source.cpp \
		src/core/ftms_codec.cpp -o $(DEBUG_TEST_BINARY)
	$(DEBUG_TEST_BINARY)

host-tests-ftms-debug:
	$(CXX) $(CXXFLAGS) -DDEBUG_WEB -DARDUINO -DSF_TEST_FTMS_DIAGNOSTICS \
		-Iinclude/core -Itests/host/debug_serial_fakes tests/host/test_ftms.cpp \
		src/core/ble_types.cpp src/core/ftms_codec.cpp src/core/ftms_source.cpp src/core/source_protocol.cpp \
		src/core/bridge_core.cpp src/core/bridge_output.cpp src/core/bt_device_adapter.cpp \
		-o /tmp/smartfitness-ftms-debug-test
	/tmp/smartfitness-ftms-debug-test

# LVGL host simulator: renders every HMI screen from fake view-model data
# into PNG files (no display needed). Requires an LVGL v9.2.2 checkout:
#   git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git $(LVGL_DIR)
LVGL_DIR ?= .cache/lvgl
SIM_BUILD ?= /tmp/smartfitness-hmi-sim
SCREENSHOT_DIR ?= screenshots
include sim/hmi_sim.mk
