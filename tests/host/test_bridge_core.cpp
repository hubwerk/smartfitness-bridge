#include "bridge/bridge_core.hpp"
#include "bridge/ftms_roles.hpp"

#include <cassert>
#include <cmath>

using namespace smartfitness;

namespace {

class FakeAdapter final : public TrainerControlAdapter {
public:
    bool accept = true;
    unsigned int calls = 0;
    ControlRequest lastRequest{};

    bool apply(const ControlRequest& request) override {
        ++calls;
        lastRequest = request;
        return accept;
    }
};

void testCapabilitiesAndSamples() {
    TrainerState state;
    state.capabilities.setMetric(Metric::Power, true);
    state.capabilities.setMetric(Metric::HeartRate, true);
    state.capabilities.setControl(Control::Incline, true);
    state.setSample(Metric::Power, 175.0F);
    state.setSample(Metric::HeartRate, 142.0F);

    assert(state.capabilities.supports(Metric::Power));
    assert(!state.capabilities.supports(Metric::Speed));
    assert(state.capabilities.supports(Control::Incline));
    assert(!state.capabilities.supports(Control::Resistance));
    assert(state.sample(Metric::Power).valid);
    assert(state.sample(Metric::Power).value == 175.0F);
}

void testFtmsRolesAreIndependent() {
    constexpr FtmsRoles inputOnly{true, false};
    constexpr FtmsRoles outputOnly{false, true};
    constexpr FtmsRoles both{true, true};
    constexpr FtmsRoles disabled{};

    static_assert(inputOnly.enabled());
    static_assert(outputOnly.enabled());
    static_assert(both.enabled());
    static_assert(!disabled.enabled());
}

void testControlGating() {
    FakeAdapter adapter;
    BridgeCore bridge(&adapter);
    TrainerState state;
    state.connected = true;
    state.capabilities.setControl(Control::Speed, true);
    state.capabilities.setControl(Control::Incline, true);
    state.capabilities.setControl(Control::Resistance, true);
    bridge.setTrainerState(state);

    assert(bridge.routeControl({Control::Speed, 5.0F}) == ControlResult::Applied);
    assert(adapter.calls == 1);
    assert(adapter.lastRequest.value == 5.0F);
    assert(bridge.routeControl({Control::Speed, -1.0F}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Speed, INFINITY}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Incline, 25.1F}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Resistance, 101.0F}) == ControlResult::InvalidValue);
    assert(bridge.routeControl({Control::Count, 0.0F}) == ControlResult::Unsupported);
    assert(adapter.calls == 1);

    adapter.accept = false;
    assert(bridge.routeControl({Control::Speed, 0.0F}) == ControlResult::AdapterRejected);
    assert(adapter.calls == 2);
}

void testDisconnectedAndMissingAdapter() {
    FakeAdapter adapter;
    BridgeCore bridge(&adapter);
    TrainerState state;
    state.capabilities.setControl(Control::Speed, true);
    bridge.setTrainerState(state);
    assert(bridge.routeControl({Control::Speed, 2.0F}) == ControlResult::Disconnected);

    state.connected = true;
    bridge.setTrainerState(state);
    assert(bridge.routeControl({Control::Speed, 2.0F}) == ControlResult::Unsupported);

    state.capabilities.setControl(Control::Speed, true);
    bridge.setTrainerState(state);
    bridge.setAdapter(nullptr);
    assert(bridge.routeControl({Control::Speed, 2.0F}) == ControlResult::NoAdapter);
}

}  // namespace

int main() {
    testCapabilitiesAndSamples();
    testFtmsRolesAreIndependent();
    testControlGating();
    testDisconnectedAndMissingAdapter();
}
