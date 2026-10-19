#include "ant/debug_log.hpp"
#include "ant_output.h"
#include "fake_ant.hpp"

#include <cassert>
#include <string>

using namespace smartfitness;
using namespace smartfitness::ant;

void testDebugAnt() {
    const Page missing{25, 0, 255, 0, 0, 255, 15, 0};
    assert(std::string(diagnostics::Summary(AntOutput::kFec, missing.data()).c_str()) == " power=- W cadence=- rpm");
    const Page power{16, 0, 255, 80, 0, 0, 150, 0};
    assert(std::string(diagnostics::Summary(AntOutput::kBicyclePower, power.data()).c_str()) ==
           " power=150.00 W cadence=80.00 rpm");
    const Page hr{128, 255, 255, 255, 0, 0, 0, 120};
    assert(std::string(diagnostics::Summary(AntOutput::kHeartRate, hr.data()).c_str()) == " hr=120.00 bpm");
    assert(std::string(diagnostics::Summary(0, power.data()).c_str()).empty());

    FakeAntTransport radio;
    AntOutput output;
    fake::now = 100;
    output.setClock([] { return fake::now; });
    output.setTransport(&radio);
    output.begin();
    output.setEnabled(true);
    for (int i = 0; i < 10; ++i) output.loop();
    assert(output.channelOpen());

    TrainerState state;
    state.connected = true;
    state.setSample(Metric::Speed, 18);
    state.setSample(Metric::Power, 150);
    state.setSample(Metric::Cadence, 80);
    state.setSample(Metric::HeartRate, 120);
    output.publishTrainerState(state);
    output.onControl([](const ControlRequest& request) {
        assert(request.control == Control::Power && request.value == 200);
        return ControlResult::Applied;
    });
    fake::now = 125;
    radio.injectPage(Page{49, 255, 255, 255, 255, 255, 0x20, 0x03});
    output.loop();
    assert(output.controlActive());
    radio.injectPage(Page{70, 255, 255, 255, 255, 0x81, 71, 1});
    radio.injectEventTx();
    output.loop();
    assert(radio.count(msg::kAcknowledgedData) == 1);

    // Completed/failed radio transfers are not a trainer's control acceptance.
    radio.inject(msg::kChannelEvent, {0, 1, code::kTransferTxCompleted});
    output.loop();
    radio.failWrites = true;
    radio.inject(msg::kChannelEvent, {0, 1, code::kTransferTxFailed});
    output.loop();
    radio.inject(msg::kBroadcastData, {0, 25});  // truncated page
    radio.inject(msg::kChannelEvent, {});       // truncated response
    radio.inject(0x99, {0xAB});                 // unknown message
    auto broken = encodeMessage(msg::kStartup, Bytes{0x20});
    broken.back() ^= 1;
    radio.incoming.insert(radio.incoming.end(), broken.begin(), broken.end());
    output.loop();
    fake::now = 5200;
    output.loop();
    assert(!output.displaySeen());
}
