// ANT serial protocol, ANT+ FE-C page codec and AntOutput (fake radio).
#include "ant/ant_message.hpp"
#include "ant/fec_codec.hpp"
#include "ant_output.h"
#include "fake_ant.hpp"

#include <cassert>
#include <cmath>
#include <set>

using namespace smartfitness;
using namespace smartfitness::ant;

namespace {

std::uint16_t u16(const Page& page, std::size_t index) {
    return static_cast<std::uint16_t>(page[index] | (page[index + 1] << 8));
}

TrainerState connectedState(MachineProfile profile) {
    TrainerState state;
    state.connected = true;
    state.trainerDetected = true;
    state.profile = profile;
    return state;
}

void testMessageCodec() {
    const Bytes frame = encodeMessage(msg::kOpenChannel, Bytes{0x00});
    assert((frame == Bytes{0xA4, 0x01, 0x4B, 0x00, static_cast<std::uint8_t>(0xA4 ^ 0x01 ^ 0x4B ^ 0x00)}));
    Page page{};
    page[0] = 16;
    const Bytes data = encodeMessage(msg::kBroadcastData, 0, page);
    assert(data.size() == 13 && data[1] == 9 && data[2] == msg::kBroadcastData && data[4] == 16);

    MessageParser parser;
    Message message;
    // Leading garbage and a corrupted frame are skipped; the next frame parses.
    Bytes stream{0x00, 0x13};
    Bytes broken = frame;
    broken.back() ^= 0xFF;
    stream.insert(stream.end(), broken.begin(), broken.end());
    stream.insert(stream.end(), data.begin(), data.end());
    int complete = 0;
    for (const auto byte : stream) {
        if (parser.feed(byte, message)) {
            ++complete;
            assert(message.id == msg::kBroadcastData && message.payload.size() == 9 && message.payload[1] == 16);
        }
    }
    assert(complete == 1 && parser.dropped() >= 1);
    // Oversized lengths are rejected.
    const Bytes oversized{kSync, 0x40, 0x4E};
    for (const auto byte : oversized) assert(!parser.feed(byte, message));
    for (const auto byte : frame) {
        if (parser.feed(byte, message)) ++complete;
    }
    assert(complete == 2 && message.id == msg::kOpenChannel);
}

void testFecBikePages() {
    fec::Encoder encoder;
    TrainerState state = connectedState(MachineProfile::IndoorBike);
    state.setSample(Metric::Speed, 36.0F);           // 10 m/s
    state.setSample(Metric::Distance, 1234.0F);
    state.setSample(Metric::HeartRate, 142.0F);
    state.setSample(Metric::ElapsedTime, 10.0F);
    state.setSample(Metric::Cadence, 90.0F);
    state.setSample(Metric::Power, 250.0F);
    state.setSample(Metric::Resistance, 40.0F);
    state.setSample(Metric::Incline, -2.5F);
    state.setSample(Metric::TotalEnergy, 300.0F);
    state.setSample(Metric::EnergyPerHour, 600.0F);
    state.setSample(Metric::MetabolicEquivalent, 7.5F);
    encoder.update(state, MachineProfile::IndoorBike, 0);

    const Page general = encoder.generalFeData();
    assert(general[0] == 16 && general[1] == 25 && general[2] == 40 && general[3] == (1234 & 0xFF));
    assert(u16(general, 4) == 10000 && general[6] == 142);
    assert((general[7] & 0x0F) == 0x07 && (general[7] >> 4) == 3);  // HR + distance, in use

    const Page settings = encoder.generalSettings();
    assert(settings[0] == 17 && static_cast<std::int16_t>(u16(settings, 4)) == -250 && settings[6] == 80);

    const Page metabolic = encoder.metabolic();
    assert(metabolic[0] == 18 && u16(metabolic, 2) == 750 && u16(metabolic, 4) == 6000 && metabolic[6] == 44);
    assert((metabolic[7] & 0x01) == 0x01);

    Page trainer = encoder.specific();
    assert(trainer[0] == 25 && trainer[1] == 1 && trainer[2] == 90 && u16(trainer, 3) == 250);
    assert(trainer[5] == (250 & 0xFF) && (trainer[6] & 0x0F) == 0);
    trainer = encoder.specific();
    assert(trainer[1] == 2 && u16(trainer, 3) == 500);

    // The broadcast cycle carries every data page the source fills.
    std::set<std::uint8_t> seen;
    for (int index = 0; index < 130; ++index) seen.insert(encoder.next(0x1234)[0]);
    assert((seen == std::set<std::uint8_t>{16, 17, 18, 25, 80, 81}));
}

void testFecFallbacksAndInvalids() {
    fec::Encoder encoder;
    TrainerState state = connectedState(MachineProfile::Rower);
    state.setSample(Metric::Pace, 125.0F);           // 4 m/s
    state.setSample(Metric::RampAngle, 45.0F);       // 100 %
    state.setSample(Metric::EnergyPerMinute, 10.0F);  // 600 kcal/h
    state.setSample(Metric::StrokeCount, 300.0F);
    state.setSample(Metric::Cadence, 28.0F);
    state.setSample(Metric::Power, 180.0F);
    encoder.update(state, MachineProfile::Rower, 0);
    encoder.update(state, MachineProfile::Rower, 2000);  // local elapsed time
    const Page general = encoder.generalFeData();
    assert(general[1] == 22 && u16(general, 4) == 4000 && general[2] == 8 && general[6] == 0xFF);
    assert((general[7] & 0x0F) == 0);
    assert(std::abs(static_cast<std::int16_t>(u16(encoder.generalSettings(), 4)) - 10000) <= 1);
    assert(u16(encoder.metabolic(), 4) == 6000 && u16(encoder.metabolic(), 2) == 0xFFFF);
    const Page rower = encoder.specific();
    assert(rower[0] == 22 && rower[3] == (300 & 0xFF) && rower[4] == 28 && u16(rower, 5) == 180);

    // Treadmill: elevation gain, no power.
    TrainerState treadmill = connectedState(MachineProfile::Treadmill);
    treadmill.setSample(Metric::PositiveElevationGain, 12.3F);
    treadmill.setSample(Metric::NegativeElevationGain, 1.0F);
    treadmill.setSample(Metric::Cadence, 160.0F);
    encoder.update(treadmill, MachineProfile::Treadmill, 3000);
    const Page page19 = encoder.specific();
    assert(page19[0] == 19 && page19[4] == 160 && page19[5] == 10 && page19[6] == 123 && (page19[7] & 0x03) == 0x03);
    // Nothing for pages 17/18: the cycle falls back to page 16.
    std::set<std::uint8_t> seen;
    for (int index = 0; index < 8; ++index) seen.insert(encoder.next(1)[0]);
    assert((seen == std::set<std::uint8_t>{16, 19}));

    // Cross trainer and climber pages.
    TrainerState cross = connectedState(MachineProfile::CrossTrainer);
    cross.setSample(Metric::StrideCount, 513.0F);
    cross.setSample(Metric::Power, 120.0F);
    encoder.update(cross, MachineProfile::CrossTrainer, 3000);
    const Page page20 = encoder.specific();
    assert(page20[0] == 20 && page20[2] == 1 && u16(page20, 5) == 120 && page20[4] == 0xFF);
    TrainerState climber = connectedState(MachineProfile::StairClimber);
    climber.setSample(Metric::StepCount, 42.0F);
    encoder.update(climber, MachineProfile::StairClimber, 3000);
    const Page page23 = encoder.specific();
    assert(page23[0] == 23 && page23[3] == 42 && u16(page23, 5) == 0xFFFF);

    // Disconnected: nothing valid, state ready.
    encoder.update(TrainerState{}, MachineProfile::IndoorBike, 4000);
    const Page idle = encoder.generalFeData();
    assert(u16(idle, 4) == 0xFFFF && idle[6] == 0xFF && (idle[7] >> 4) == 2 && idle[2] == 0);
}

void testFecControlPages() {
    Page basic{48, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 100};
    auto control = fec::decodeControlPage(basic);
    assert(control.known && control.hasRequest && control.request.control == Control::Resistance);
    assert(control.request.value == 50.0F);
    Page power{49, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x03};  // 800 * 0.25 = 200 W
    control = fec::decodeControlPage(power);
    assert(control.request.control == Control::Power && control.request.value == 200.0F);
    Page track{51, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0, 0xFF};
    const std::uint16_t raw = 20000 + 350;  // +3.5 %
    track[5] = raw & 0xFF;
    track[6] = raw >> 8;
    control = fec::decodeControlPage(track);
    assert(control.request.control == Control::Incline && std::abs(control.request.value - 3.5F) < 0.01F);
    Page wind{50, 0, 0, 0, 0, 0, 0, 0};
    control = fec::decodeControlPage(wind);
    assert(control.known && !control.hasRequest);
    Page unknown{99, 0, 0, 0, 0, 0, 0, 0};
    assert(!fec::decodeControlPage(unknown).known);
    assert(fec::commandStatusFor(ControlResult::Applied) == fec::CommandStatus::Pass);
    assert(fec::commandStatusFor(ControlResult::Unsupported) == fec::CommandStatus::NotSupported);
    assert(fec::commandStatusFor(ControlResult::InvalidValue) == fec::CommandStatus::Rejected);
    assert(fec::commandStatusFor(ControlResult::AdapterRejected) == fec::CommandStatus::Fail);
}

void testAntOutputLifecycle() {
    std::uint32_t now = 0;
    FakeAntTransport radio;
    AntOutput ant;
    ant.setClock([&now] { return now; });
    assert(!ant.available() && ant.link() == AntOutput::Link::NoHardware);
    ant.setTransport(&radio);
    ant.setDeviceNumber(0x1234);
    ant.begin();
    ant.setEnabled(true);
    assert(ant.available() && std::string(ant.hardware()) == "fake");

    // Reset -> startup -> network key, assign, channel id, period, RF, open.
    ant.loop();
    for (int index = 0; index < 4; ++index) ant.loop();
    assert(ant.link() == AntOutput::Link::Open && ant.channelOpen());
    const std::vector<std::uint8_t> sequence{msg::kResetSystem,   msg::kSetNetworkKey, msg::kAssignChannel,
                                             msg::kChannelId,     msg::kChannelPeriod, msg::kChannelRfFrequency,
                                             msg::kOpenChannel};
    for (std::size_t index = 0; index < sequence.size(); ++index) assert(radio.written[index].id == sequence[index]);
    const auto& channelId = radio.written[3].payload;
    assert((channelId == Bytes{0, 0x34, 0x12, fec::kDeviceType, fec::kTransmissionType}));
    assert(radio.written[2].payload[1] == fec::kChannelTypeMaster);
    assert((radio.written[4].payload == Bytes{0, 0x00, 0x20}) && radio.written[5].payload[1] == 57);
    assert(ant.pagesSent() == 1 && radio.pages().back()[0] == 16);

    // Live data: every EVENT_TX sends the next page.
    TrainerState state = connectedState(MachineProfile::IndoorBike);
    state.capabilities.setControl(Control::Power, true);
    state.setSample(Metric::Power, 210.0F);
    state.setSample(Metric::Cadence, 85.0F);
    state.setSample(Metric::HeartRate, 130.0F);
    ant.publishTrainerState(state);
    for (int index = 0; index < 3; ++index) {
        radio.injectEventTx();
        ant.loop();
    }
    assert(ant.pagesSent() == 4);
    const auto pages = radio.pages();
    assert(pages[1][0] == 25 && u16(pages[1], 5) == 210 && pages[1][2] == 85);
    assert(pages[2][0] == 16 && pages[2][6] == 130);

    // Target power from the display goes through the control gate.
    ControlRequest received{Control::Speed, 0.0F};
    ant.onControl([&received](const ControlRequest& request) {
        received = request;
        return ControlResult::Applied;
    });
    radio.injectPage(Page{49, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x00});  // 240 * 0.25 = 60 W
    ant.loop();
    assert(received.control == Control::Power && received.value == 60.0F);
    assert(ant.displaySeen() && ant.controlActive());
    // Page 70 asks for the command status twice.
    radio.injectPage(Page{70, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 71, 0x01});
    for (int index = 0; index < 3; ++index) {
        radio.injectEventTx();
        ant.loop();
    }
    const auto replies = radio.pages();
    const Page status = replies[replies.size() - 3];
    assert(status[0] == 71 && status[1] == 49 && status[3] == 0 && status[6] == 0xF0);
    assert(replies[replies.size() - 2][0] == 71 && replies.back()[0] != 71);
    now += AntOutput::kDisplaySeenMs;
    assert(!ant.displaySeen() && !ant.controlActive());

    // Unplug: waits for the stick, then reconfigures on its return.
    radio.attached = false;
    ant.loop();
    assert(ant.link() == AntOutput::Link::WaitingForStick && !ant.stickConnected());
    radio.attached = true;
    radio.written.clear();
    for (int index = 0; index < 4; ++index) ant.loop();
    assert(ant.channelOpen() && radio.written.front().id == msg::kResetSystem);

    // Disabling closes the channel and resets the stick.
    radio.written.clear();
    ant.setEnabled(false);
    assert(radio.count(msg::kCloseChannel) == 1 && radio.count(msg::kResetSystem) == 1);
    assert(ant.link() == AntOutput::Link::Disabled && !ant.channelOpen());
}

void testAntOutputTimeoutsAndErrors() {
    std::uint32_t now = 0;
    FakeAntTransport radio;
    radio.autoRespond = false;
    AntOutput ant;
    ant.setClock([&now] { return now; });
    ant.setTransport(&radio);
    ant.begin();
    ant.setEnabled(true);
    ant.loop();
    assert(ant.link() == AntOutput::Link::Resetting && radio.count(msg::kResetSystem) == 1);
    // Sticks without a startup message continue after the reset timeout.
    now += AntOutput::kResetTimeoutMs;
    ant.loop();
    assert(ant.link() == AntOutput::Link::Configuring && radio.written.back().id == msg::kSetNetworkKey);
    // No response: retry with a fresh reset after the retry delay.
    now += AntOutput::kResponseTimeoutMs;
    ant.loop();
    assert(ant.link() == AntOutput::Link::Resetting);
    now += AntOutput::kRetryMs;
    ant.loop();
    assert(radio.count(msg::kResetSystem) == 2);
    // An error response also restarts the configuration.
    radio.inject(msg::kStartup, {0x20});
    ant.loop();
    assert(radio.written.back().id == msg::kSetNetworkKey);
    radio.inject(msg::kChannelEvent, {0x00, msg::kSetNetworkKey, 0x28});
    ant.loop();
    assert(ant.link() == AntOutput::Link::Resetting && !ant.channelOpen());
    // Write failures never open the channel.
    radio.autoRespond = true;
    radio.failWrites = true;
    now += AntOutput::kRetryMs;
    ant.loop();
    assert(!ant.channelOpen());
    radio.failWrites = false;
    now += AntOutput::kRetryMs;
    for (int index = 0; index < 4; ++index) ant.loop();
    assert(ant.channelOpen());
    // Channel closed by the stick: reconfigure.
    radio.inject(msg::kChannelEvent, {0x00, 0x01, code::kChannelClosed});
    ant.loop();
    assert(!ant.channelOpen());
}

}  // namespace

void runAntTests() {
    testMessageCodec();
    testFecBikePages();
    testFecFallbacksAndInvalids();
    testFecControlPages();
    testAntOutputLifecycle();
    testAntOutputTimeoutsAndErrors();
}
