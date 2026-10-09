CXX = g++
HOST_TEST_SOURCES = tests/host/test_bridge_core.cpp tests/host/test_ftms.cpp tests/host/test_config_store.cpp \
	tests/host/test_ble_input.cpp \
	src/bridge_core.cpp src/bt_device_adapter.cpp src/bridge_output.cpp \
	src/config_store.cpp src/config_backend.cpp src/web_ui.cpp src/ftms_codec.cpp src/ftms_output.cpp \
	src/ble_types.cpp src/sensor_codec.cpp src/source_protocol.cpp src/ftms_source.cpp src/sensor_sources.cpp \
	src/protocols/echelon.cpp src/protocols/domyos.cpp src/protocols/yesoul_keiser.cpp src/protocols/registry.cpp \
	src/ble_input_adapter.cpp
HOST_TEST_BINARY = /tmp/smartfitness-core-test

.PHONY: host-tests
host-tests:
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Iinclude $(HOST_TEST_SOURCES) -o $(HOST_TEST_BINARY)
	$(HOST_TEST_BINARY)
