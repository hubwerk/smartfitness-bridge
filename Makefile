CXX = g++
HOST_TEST_SOURCES = tests/host/test_bridge_core.cpp tests/host/test_ftms.cpp tests/host/test_config_store.cpp \
	tests/host/test_ble_input.cpp tests/host/test_web.cpp \
	tests/host/test_ifit_bowflex.cpp tests/host/test_trainers_rower.cpp tests/host/test_treadmills.cpp \
	src/bridge_core.cpp src/bt_device_adapter.cpp src/bridge_output.cpp \
	src/config_store.cpp src/config_backend.cpp src/web_ui.cpp src/bridge_commands.cpp src/web_api.cpp src/wifi_policy.cpp src/ftms_codec.cpp src/ftms_output.cpp \
	src/ble_types.cpp src/sensor_codec.cpp src/source_protocol.cpp src/ftms_source.cpp src/sensor_sources.cpp \
	src/protocols/echelon.cpp src/protocols/domyos.cpp src/protocols/yesoul_keiser.cpp src/protocols/registry.cpp \
	src/protocols/ifit_bowflex.cpp src/protocols/trainers_rower.cpp src/protocols/treadmills.cpp \
	src/ble_input_adapter.cpp
HOST_TEST_BINARY = /tmp/smartfitness-core-test

.PHONY: host-tests
host-tests:
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Iinclude -Itests/host/third_party $(HOST_TEST_SOURCES) -o $(HOST_TEST_BINARY)
	$(HOST_TEST_BINARY)
