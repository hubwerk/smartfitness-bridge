// ANT serial protocol, ANT+ FE-C page codec and AntOutput (fake radio).
#include "ant/ant_message.hpp"
#include "ant/ant_uart_transport.hpp"
#include "ant/fec_codec.hpp"
#include "ant_output.h"
#include "fake_ant.hpp"

#include <cassert>
#include <cmath>
#include <set>
#include <algorithm>
#include <limits>

using namespace smartfitness;
using namespace smartfitness::ant;

namespace {

using CapableRadio = FakeAntTransport;

class FakeSerial : public AntSerial {
public:
    FakeAntTransport radio;
    bool ready = true;
    unsigned begins = 0;
    std::size_t rxLimit = 128, txLimit = 512;
    MessageParser parser;

    bool begin() override { ++begins; return ready; }
    std::size_t read(std::uint8_t* data, std::size_t capacity) override {
        return radio.attached ? radio.read(data, std::min(capacity, rxLimit)) : 0;
    }
    std::size_t write(const std::uint8_t* data, std::size_t length) override {
        const auto count = std::min(length, txLimit);
        if (!radio.attached) return count;  // A UART cannot sense the physical cable.
        Message message;
        for (std::size_t i = 0; i < count; ++i) {
            if (parser.feed(data[i], message)) {
                const auto frame = encodeMessage(message.id, message.payload);
                radio.write(frame.data(), frame.size());
            }
        }
        return count;
    }
};

void testUartDetection() {
    std::uint32_t now = 0;
    FakeSerial serial;
    serial.radio.autoRespond = false;
    AntUartTransport uart(serial, [&] { return now; });
    assert(!uart.connected() && uart.channelLimit() == 0);
    const auto data = encodeMessage(msg::kOpenChannel, Bytes{0});
    assert(!uart.write(data.data(), data.size()));
    serial.ready = false;
    assert(!uart.begin());
    serial.ready = true;
    assert(uart.begin() && !uart.connected());
    assert(serial.radio.count(msg::kResetSystem) == 1);
    serial.radio.incoming = {0, 0xFF, 0x31, kSync, 0xFF};
    auto broken = encodeMessage(msg::kStartup, Bytes{0x20});
    broken.back() ^= 1;
    serial.radio.incoming.insert(serial.radio.incoming.end(), broken.begin(), broken.end());
    serial.radio.inject(msg::kStartup, {0x20});
    serial.rxLimit = 1;
    for (int i = 0; i < 15; ++i) {
        uart.begin();
        now += 2;  // Fragmented frames at ordinary serial timing.
    }
    assert(uart.connected() && serial.begins == 2);
    serial.radio.inject(0x54, {8, 1, 0, 0, 0});
    serial.radio.inject(0x3E, {'A', 'P', '2', 0, 'x'});
    for (int i = 0; i < 30; ++i) uart.begin();
    assert(uart.channelLimit() == 8);
    assert(std::string(uart.description()) == "UART ANT module · AP2 (8 ch)");
    std::uint8_t bytes[128];
    MessageParser parser;
    Message message;
    unsigned frames = 0;
    const auto count = uart.read(bytes, sizeof(bytes));
    for (std::size_t i = 0; i < count; ++i) frames += parser.feed(bytes[i], message);
    assert(frames == 3);  // Startup, capabilities and version reach AntOutput.
    assert(uart.read(bytes, sizeof(bytes)) == 0);
    now += AntUartTransport::kDisconnectMs;
    uart.begin();
    assert(!uart.connected() && uart.channelLimit() == 0);
    assert(std::string(uart.description()) == "UART ANT module");
    assert(!uart.write(data.data(), data.size()));
    serial.radio.inject(msg::kStartup, {0x20});
    for (int i = 0; i < 5; ++i) uart.begin();
    assert(uart.connected());
}

void testUartRetryAndTiming() {
    std::uint32_t now = std::numeric_limits<std::uint32_t>::max() - 999;
    FakeSerial serial;
    serial.radio.autoRespond = false;
    AntUartTransport uart(serial, [&] { return now; });
    uart.begin();
    now += 1000;
    uart.begin();
    assert(serial.radio.count(0x4D) == 1 && !uart.connected());
    now += 1000;
    uart.begin();
    assert(serial.radio.count(msg::kResetSystem) == 2);
    // A stale, incomplete frame must not consume the next startup message.
    serial.radio.incoming = {kSync, 20, msg::kBroadcastData, 0};
    uart.begin();
    now += AntUartTransport::kInterByteTimeoutMs;
    serial.radio.inject(msg::kStartup, {});  // Valid checksum, invalid startup payload.
    uart.begin();
    assert(!uart.connected());
    serial.radio.inject(0x54, {0, 1, 0, 0});
    uart.begin();
    assert(!uart.connected());
    // Radios without a StartupMessage can be discovered through capabilities.
    serial.radio.inject(0x54, {2, 1, 0, 0});
    uart.begin();
    assert(uart.connected() && uart.channelLimit() == 2);
    now += AntUartTransport::kRetryMs;
    uart.begin();
    assert(serial.radio.count(msg::kResetSystem) == 2);  // Heartbeats never reset channels.
    serial.radio.inject(0x54, {2, 1, 0, 0});
    uart.begin();
    now += AntUartTransport::kDisconnectMs - 1;
    uart.begin();
    assert(uart.connected());
    ++now;
    uart.begin();
    assert(!uart.connected());
}

void testUartBuffersAndOutput() {
    std::uint32_t now = 0;
    FakeSerial serial;
    AntUartTransport uart(serial, [&] { return now; });
    uart.begin();
    uart.begin();
    assert(uart.connected());
    for (int i = 0; i < 4; ++i) uart.begin();
    serial.txLimit = 0;  // Simulates FIFO full or nRTS not ready.
    const auto frame = encodeMessage(msg::kBroadcastData, 0, Page{16});
    unsigned queued = 0;
    while (uart.write(frame.data(), frame.size())) ++queued;
    assert(queued == 512 / frame.size());
    uart.begin();
    const auto before = serial.radio.count(msg::kBroadcastData);
    serial.txLimit = 1;  // Partial writes, including ring wraparound.
    for (unsigned i = 0; i < queued * frame.size(); ++i) uart.begin();
    assert(serial.radio.count(msg::kBroadcastData) == before + queued);
    assert(!uart.write(nullptr, 3) && !uart.write(frame.data(), 0));
    std::array<std::uint8_t, 513> oversized{};
    assert(!uart.write(oversized.data(), oversized.size()));

    // The real AntOutput negotiates channel limits through this byte stream.
    serial.txLimit = 512;
    AntOutput ant;
    ant.setClock([&] { return now; });
    ant.setTransport(&uart);
    ant.setServices(AntOutput::kFec | AntOutput::kHeartRate);
    ant.setEnabled(true);
    ant.begin();
    TrainerState state;
    state.connected = true;
    state.trainerDetected = true;
    state.profile = MachineProfile::IndoorBike;
    ant.publishTrainerState(state);
    for (int i = 0; i < 40; ++i) ant.loop();
    assert(ant.channelOpen(AntOutput::kFec) && ant.channelOpen(AntOutput::kHeartRate));
    assert(ant.channelLimit() == 8 && uart.channelLimit() == 8);
    serial.radio.injectEventTx();
    serial.radio.inject(msg::kChannelEvent, {1, 1, code::kEventTx});
    for (int i = 0; i < 4; ++i) ant.loop();
    assert(ant.pagesSent() >= 2);
    serial.radio.attached = false;
    now += AntUartTransport::kDisconnectMs;
    ant.loop();
    assert(!ant.stickConnected() && !ant.channelOpen());
    serial.radio.attached = true;
    now += AntUartTransport::kRetryMs;
    for (int i = 0; i < 40; ++i) ant.loop();
    assert(ant.channelOpen(AntOutput::kFec) && ant.channelOpen(AntOutput::kHeartRate));
    serial.radio.channels = 1;
    ant.setServices(AntOutput::kFec | AntOutput::kBicyclePower | AntOutput::kHeartRate);
    for (int i = 0; i < 40; ++i) ant.loop();
    assert(ant.error() == AntOutput::Error::InsufficientChannels && !ant.channelOpen());
}

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
    MessageParser truncatedParser;
    Bytes truncated{kSync, 0x01, msg::kResetSystem, 0x00};  // Missing checksum before the next frame.
    truncated.insert(truncated.end(), frame.begin(), frame.end());
    bool recovered = false;
    for (const auto byte : truncated) {
        if (truncatedParser.feed(byte, message)) {
            recovered = true;
            assert(message.id == msg::kOpenChannel);
        }
    }
    assert(recovered && truncatedParser.dropped() == 1);
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
    assert(settings[0] == 17 && static_cast<std::int16_t>(u16(settings, 4)) == -250 && settings[6] == 0xFF);

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
    assert(rower[0] == 22 && rower[3] == (300 & 0xFF) && rower[4] == 28 &&
           (rower[7] & 0x01) == 0x01 && u16(rower, 5) == 180);

    // Treadmill: elevation gain, no power.
    TrainerState treadmill = connectedState(MachineProfile::Treadmill);
    treadmill.setSample(Metric::PositiveElevationGain, 12.3F);
    treadmill.setSample(Metric::NegativeElevationGain, 1.0F);
    treadmill.setSample(Metric::Cadence, 160.0F);
    encoder.update(treadmill, MachineProfile::Treadmill, 3000);
    const Page page19 = encoder.specific();
    assert(page19[0] == 19 && page19[4] == 160 && page19[5] == 10 && page19[6] == 123 && (page19[7] & 0x03) == 0x03);
    treadmill.setSample(Metric::NegativeElevationGain, 0.0F, false);
    encoder.update(treadmill, MachineProfile::Treadmill, 3000);
    assert((encoder.specific()[7] & 0x03) == 0x01);
    treadmill.setSample(Metric::PositiveElevationGain, 0.0F, false);
    treadmill.setSample(Metric::NegativeElevationGain, 1.0F);
    encoder.update(treadmill, MachineProfile::Treadmill, 3000);
    assert((encoder.specific()[7] & 0x03) == 0x02);
    // Nothing for pages 17/18: the cycle falls back to page 16.
    std::set<std::uint8_t> seen;
    for (int index = 0; index < 8; ++index) seen.insert(encoder.next(1)[0]);
    assert((seen == std::set<std::uint8_t>{16, 19}));

    // Cross trainer and climber pages.
    TrainerState cross = connectedState(MachineProfile::CrossTrainer);
    cross.setSample(Metric::StrideCount, 513.0F);
    cross.setSample(Metric::PositiveElevationGain, 12.3F);
    cross.setSample(Metric::Power, 120.0F);
    encoder.update(cross, MachineProfile::CrossTrainer, 3000);
    const Page page20 = encoder.specific();
    assert(page20[0] == 20 && page20[2] == 123 && page20[3] == 1 && (page20[7] & 0x03) == 0x03);
    assert(u16(page20, 5) == 120 && page20[4] == 0xFF);
    cross.setSample(Metric::StrideCount, 0.0F, false);
    encoder.update(cross, MachineProfile::CrossTrainer, 3000);
    const Page elevationOnly = encoder.specific();
    assert(elevationOnly[2] == 123 && elevationOnly[3] == 0 && (elevationOnly[7] & 0x03) == 0x02);
    cross.setSample(Metric::PositiveElevationGain, 0.0F, false);
    cross.setSample(Metric::StrideCount, 7.0F);
    encoder.update(cross, MachineProfile::CrossTrainer, 3000);
    const Page strideOnly = encoder.specific();
    assert(strideOnly[2] == 0 && strideOnly[3] == 7 && (strideOnly[7] & 0x03) == 0x01);
    cross.setSample(Metric::StrideCount, 0.0F, false);
    encoder.update(cross, MachineProfile::CrossTrainer, 3000);
    assert((encoder.specific()[7] & 0x03) == 0);
    TrainerState rowerMissing = connectedState(MachineProfile::Rower);
    encoder.update(rowerMissing, MachineProfile::Rower, 3000);
    assert((encoder.specific()[7] & 0x01) == 0);
    TrainerState climber = connectedState(MachineProfile::StairClimber);
    climber.setSample(Metric::StepCount, 42.0F);
    encoder.update(climber, MachineProfile::StairClimber, 3000);
    const Page page23 = encoder.specific();
    assert(page23[0] == 23 && page23[3] == 42 && (page23[7] & 0x01) == 0x01 && u16(page23, 5) == 0xFFFF);
    climber.setSample(Metric::StrideCount, 7.0F);
    encoder.update(climber, MachineProfile::StairClimber, 3000);
    assert(encoder.specific()[3] == 7 && (encoder.specific()[7] & 0x01) == 0x01);
    climber.setSample(Metric::StrideCount, 0.0F, false);
    climber.setSample(Metric::StepCount, 0.0F, false);
    encoder.update(climber, MachineProfile::StairClimber, 3000);
    assert((encoder.specific()[7] & 0x01) == 0);

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
    CapableRadio radio;
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
    const std::vector<std::uint8_t> sequence{msg::kResetSystem, 0x4D, msg::kSetNetworkKey, msg::kAssignChannel,
                                             msg::kChannelId,     msg::kChannelPeriod, msg::kChannelRfFrequency,
                                             msg::kOpenChannel};
    for (std::size_t index = 0; index < sequence.size(); ++index) assert(radio.written[index].id == sequence[index]);
    const auto& channelId = radio.written[4].payload;
    assert((channelId == Bytes{0, 0x34, 0x12, fec::kDeviceType, fec::kTransmissionType}));
    assert(radio.written[3].payload[1] == fec::kChannelTypeMaster);
    assert((radio.written[5].payload == Bytes{0, 0x00, 0x20}) && radio.written[6].payload[1] == 57);
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
    radio.injectPage(Page{70, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 71, 0x01});
    ant.loop();
    radio.injectEventTx();
    ant.loop();
    assert(radio.written.back().id == msg::kAcknowledgedData && radio.written.back().payload[1] == 71);
    radio.injectEventTx();
    ant.loop();
    assert(radio.written.back().id == msg::kBroadcastData);
    // Back-to-back requests queue: each page is sent as often as requested.
    radio.injectPage(Page{70, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 80, 0x01});
    ant.loop();
    radio.injectPage(Page{70, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 54, 0x01});
    ant.loop();
    for (int index = 0; index < 5; ++index) {
        radio.injectEventTx();
        ant.loop();
    }
    const auto queued = radio.pages();
    const std::size_t first = queued.size() - 5;
    assert(queued[first][0] == 80 && queued[first + 1][0] == 80);
    assert(queued[first + 2][0] == 54 && queued[first + 3][0] == 54);
    assert(queued[first + 4][0] != 80 && queued[first + 4][0] != 54);
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
    CapableRadio radio;
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
    assert(ant.link() == AntOutput::Link::Configuring && radio.written.back().id == 0x4D);
    // No response: retry with a fresh reset after the retry delay.
    now += AntOutput::kResponseTimeoutMs;
    ant.loop();
    assert(ant.link() == AntOutput::Link::Resetting);
    assert(ant.error() == AntOutput::Error::CapabilitiesUnavailable);
    now += AntOutput::kRetryMs;
    ant.loop();
    assert(radio.count(msg::kResetSystem) == 2);
    // An error response also restarts the configuration.
    radio.inject(msg::kStartup, {0x20});
    ant.loop();
    radio.inject(0x54, {8, 1, 0, 0, 0});
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

void testSensorEncoders() {
    sensor::Encoder encoder;
    TrainerState state = connectedState(MachineProfile::Treadmill);
    encoder.update(state, 0);
    assert(encoder.bicyclePower()[3] == 0xFF && u16(encoder.bicyclePower(), 6) == 0xFFFF);
    assert(encoder.heartRate()[7] == 0);
    assert(encoder.speedCadence() == Page{});
    assert(encoder.stride()[7] == 0xFF);
    state.setSample(Metric::Cadence, 120);
    state.setSample(Metric::Speed, 7.578F);  // One 2.105 m wheel revolution/s.
    state.setSample(Metric::HeartRate, 120);
    state.setSample(Metric::Power, 250);
    encoder.update(state, 0);
    encoder.update(state, 1000);
    const auto sc = encoder.speedCadence();
    assert(u16(sc, 0) == 1024 && u16(sc, 2) == 2);
    assert(u16(sc, 6) == 1 && std::abs(static_cast<int>(u16(sc, 4)) - 1024) <= 1);
    const auto hr = encoder.heartRate();
    assert(u16(hr, 4) == 1024 && hr[6] == 2 && hr[7] == 120);
    const auto power = encoder.bicyclePower();
    assert(power[0] == 16 && power[1] == 1 && power[2] == 0xFF && power[3] == 120);
    assert(u16(power, 4) == 250 && u16(power, 6) == 250);
    const auto stride = encoder.stride();
    assert(stride[0] == 1 && stride[1] == 0 && stride[2] == 1 && stride[6] == 1);
    assert((stride[4] & 15) == 2 && stride[5] == 27);
    assert((stride == Page{1, 0, 1, 2, 0x12, 27, 1, 0xFF}));
    encoder.update(state, 65000);
    assert(u16(encoder.speedCadence(), 0) == 1024);
    assert(encoder.heartRate()[6] == 130);
    for (int i = 0; i < 262; ++i) encoder.bicyclePower();
    assert(u16(encoder.bicyclePower(), 4) == static_cast<std::uint16_t>(264 * 250));
    state.setSample(Metric::Cadence, 0);
    state.setSample(Metric::Speed, 0);
    state.setSample(Metric::HeartRate, 0);
    encoder.update(state, 65000);
    const auto stopped = encoder.speedCadence();
    encoder.update(state, 66000);
    assert(encoder.speedCadence() == stopped);
    state.setSample(Metric::Power, NAN);
    state.setSample(Metric::Cadence, INFINITY);
    encoder.update(state, 66000);
    assert(u16(encoder.bicyclePower(), 6) == 0xFFFF && encoder.bicyclePower()[3] == 0xFF);
    encoder.update(TrainerState{}, 66000);
    assert(encoder.heartRate()[7] == 0);
    assert(u16(encoder.bicyclePower(), 6) == 0xFFFF);
    encoder.reset();
    state.setSample(Metric::Cadence, 120);
    state.setSample(Metric::HeartRate, 120);
    state.setSample(Metric::Speed, 7.578F);
    encoder.update(state, 0);
    encoder.update(state, 32768000);
    assert(u16(encoder.speedCadence(), 2) == 0);  // 65536 crank revolutions.
    assert(encoder.heartRate()[6] == 0 && encoder.stride()[6] == 0);
    encoder.reset();
    encoder.update(state, 0xFFFFFF00U);
    encoder.update(state, 744);  // One second across the uint32 millisecond wrap.
    const auto wrapSc = encoder.speedCadence();
    assert(u16(wrapSc, 2) == 2);
    assert(u16(wrapSc, 0) == static_cast<std::uint16_t>((std::uint64_t{0xFFFFFF00U} + 1000) * 1024 / 1000));
    // Read-only sensor sources are connected but are not controllable trainers.
    encoder.reset();
    state.trainerDetected = false;
    state.setSample(Metric::Power, 200);
    encoder.update(state, 0);
    encoder.update(state, 1000);
    assert(encoder.heartRate()[7] == 120 && encoder.heartRate()[6] == 2);
    assert(u16(encoder.bicyclePower(), 6) == 200);
    assert(u16(encoder.speedCadence(), 2) == 2 && u16(encoder.speedCadence(), 6) == 1);
    assert(encoder.stride()[2] == 1 && encoder.stride()[3] == 2);
    encoder.reset();
    for (int i = 0; i < 12; ++i)
        assert((encoder.heartRate()[0] >> 7) == ((i / 4) & 1));
    encoder.reset();
    state.setSample(Metric::Speed, 9);  // 2.5 m/s, exactly representable.
    encoder.update(state, 0);
    encoder.update(state, 257250);
    // Seconds and stride count wrap at 256; distance wraps at 256 metres.
    // Byte 4 combines 1/16 m fractional distance and integer m/s.
    assert((encoder.stride() == Page{1, 50, 1, 131, 0x22, 0x80, 1, 0xFF}));
    encoder.update(TrainerState{}, 257250);
    const auto disconnectedStride = encoder.stride();
    assert((disconnectedStride[4] & 15) == 0 && disconnectedStride[5] == 0);
    encoder.update(TrainerState{}, 258250);
    assert(encoder.stride() == disconnectedStride);
}

void testSelectableChannels() {
    CapableRadio radio;
    AntOutput ant;
    std::uint32_t now = 0;
    ant.setTransport(&radio);
    ant.setClock([&now] { return now; });
    ant.setProfileOverride(FtmsOutputProfile::Treadmill);
    ant.setServices(31);
    ant.setDeviceNumber(123);
    ant.begin();
    for (int i = 0; i < 20; ++i) ant.loop();
    assert(ant.services() == 31 && ant.configuredServices() == 31 && ant.channelLimit() == 8);
    assert(ant.link() == AntOutput::Link::Open && radio.count(msg::kSetNetworkKey) == 1);
    assert(radio.count(msg::kOpenChannel) == 5);
    const std::uint8_t types[] = {17, 11, 120, 121, 124};
    const std::uint16_t periods[] = {8192, 8182, 8070, 8086, 8134};
    for (const auto& message : radio.written) {
        if (message.id == msg::kChannelId) {
            assert(message.payload[1] == 123 && message.payload[2] == 0);
            assert(message.payload[3] == types[message.payload[0]]);
        }
        if (message.id == msg::kChannelPeriod) {
            assert((message.payload[1] | (message.payload[2] << 8)) == periods[message.payload[0]]);
        }
    }
    for (std::uint8_t channel = 0; channel < 5; ++channel) {
        assert(ant.channelOpen(1 << channel));
        const auto before = ant.pagesSent();
        radio.inject(msg::kChannelEvent, {channel, 1, code::kEventTx});
        ant.loop();
        assert(ant.pagesSent() == before + 1 && radio.written.back().payload[0] == channel);
    }
    bool controlled = false;
    ant.onControl([&controlled](const ControlRequest&) { controlled = true; return ControlResult::Applied; });
    radio.inject(msg::kAcknowledgedData, {1, 49, 255, 255, 255, 255, 255, 240, 0});
    ant.loop();
    assert(!controlled && !ant.displaySeen());
    ant.setServices(4);
    for (int i = 0; i < 10; ++i) ant.loop();
    assert(ant.channelOpen(4) && !ant.channelOpen(1) && radio.written.back().payload[0] == 0);
    radio.injectPage(Page{49, 255, 255, 255, 255, 255, 240, 0});
    ant.loop();
    assert(!controlled && !ant.displaySeen());  // Channel zero is now HR, not FE-C.
    ant.setServices(31);
    radio.channels = 2;
    for (int i = 0; i < 10; ++i) ant.loop();
    assert(ant.error() == AntOutput::Error::InsufficientChannels && !ant.channelOpen());
    assert(std::string(AntOutput::errorName(ant.error())) == "insufficient-channels");
    now += AntOutput::kRetryMs;
    radio.channels = 8;
    for (int i = 0; i < 20; ++i) ant.loop();
    assert(ant.channelOpen(16));
    ant.setProfileOverride(FtmsOutputProfile::IndoorBike);
    for (int i = 0; i < 20; ++i) ant.loop();
    assert(ant.services() == 15 && !ant.channelOpen(16));
    ant.setServices(0);
    for (int i = 0; i < 10; ++i) ant.loop();
    assert(ant.services() == 1 && ant.configuredServices() == 0 && ant.channelOpen(1));
    ant.setEnabled(false);
    const auto writes = radio.written.size();
    radio.inject(msg::kStartup, {0x20});
    ant.loop();
    assert(radio.written.size() == writes && !ant.channelOpen());
}

}  // namespace

void runAntTests() {
    testUartDetection();
    testUartRetryAndTiming();
    testUartBuffersAndOutput();
    testMessageCodec();
    testFecBikePages();
    testFecFallbacksAndInvalids();
    testFecControlPages();
    testAntOutputLifecycle();
    testAntOutputTimeoutsAndErrors();
    testSensorEncoders();
    testSelectableChannels();
}
