CXX = g++
HOST_TEST_SOURCES = tests/host/test_bridge_core.cpp tests/host/test_ftms.cpp \
	src/bridge_core.cpp src/bt_device_adapter.cpp src/bridge_output.cpp \
	src/config_store.cpp src/web_ui.cpp src/ftms_codec.cpp src/ftms_output.cpp
HOST_TEST_BINARY = /tmp/smartfitness-core-test

.PHONY: host-tests
host-tests:
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Iinclude $(HOST_TEST_SOURCES) -o $(HOST_TEST_BINARY)
	$(HOST_TEST_BINARY)
