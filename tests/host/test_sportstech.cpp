#include "protocols/sportstech.hpp"
#include "protocols/registry.hpp"

#include <cassert>
#include <cmath>
#include <limits>

namespace {
using namespace smartfitness;
using namespace smartfitness::protocols;
using ble::Bytes;
using sportstech::Mode;

bool near(float a, float b) { return std::fabs(a - b) < 0.001F; }

void notify(SourceProtocol& source, const Bytes& frame, std::uint32_t now) {
    source.onNotify(0, frame.data(), frame.size(), now);
}

// Synthetic running frame in the captured 17-byte layout.
Bytes running(std::uint8_t speedTenths, std::uint8_t incline = 0, std::uint8_t heartRate = 0,
              std::uint8_t mode = 0x03) {
    Bytes payload{mode, speedTenths, incline, 0x10, 0x00, 0x25, 0x00, 0x0e, 0x00, 0x00, 0x00, heartRate, 0x00};
    Bytes out = sportstech::frame(0x51, payload);
    assert(out.size() == 17);
    return out;
}

void codecTests() {
    assert(sportstech::pollFrame() == Bytes({0x02, 0x51, 0x51, 0x03}));
    // Upstream button/__init__.py stop command and captured app commands.
    assert(sportstech::stopFrame() == Bytes({0x02, 0x53, 0x03, 0x50, 0x03}));
    assert(sportstech::frame(0x53, {0x01, 0, 0, 0, 0, 0, 0, 0, 0}) ==
           Bytes({0x02, 0x53, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0x52, 0x03}));
    assert(sportstech::frame(0x50, {0x02}) == Bytes({0x02, 0x50, 0x02, 0x52, 0x03}));
    assert(sportstech::speedInclineFrame(80, 5) ==
           Bytes({0x02, 0x53, 0x02, 80, 5, 0, 0, 0, 0, 0, 0, 0, 0x53 ^ 0x02 ^ 80 ^ 5, 0x03}));

    // Hardware capture from upstream docs/protocol.md: 6.0 km/h, 28 s, 42 m, 1.6 kcal.
    const Bytes capture{0x02, 0x51, 0x03, 0x3c, 0x00, 0x1c, 0x00, 0x2a, 0x00, 0x10,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x03};
    sportstech::Status status;
    assert(sportstech::decodeStatus(capture.data(), capture.size(), status));
    assert(status.full && status.mode == Mode::Running);
    assert(near(status.speed, 6.0F) && near(status.incline, 0));
    assert(status.elapsed == 28 && status.distance == 42 && near(status.calories, 1.6F) && status.heartRate == 0);

    const Bytes stopping{0x02, 0x51, 0x04, 0x03, 0x00, 0x16, 0x01, 0x48, 0x00, 0x1c,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x03};
    assert(sportstech::decodeStatus(stopping.data(), stopping.size(), status));
    assert(status.mode == Mode::Stopping && near(status.speed, 0.3F) && status.elapsed == 278 &&
           status.distance == 72 && near(status.calories, 2.8F));

    const Bytes stopped{0x02, 0x51, 0x00, 0x51, 0x03};
    assert(sportstech::decodeStatus(stopped.data(), stopped.size(), status));
    assert(!status.full && status.mode == Mode::Stopped);

    // Checksum, framing, truncation and non-status frames are rejected.
    Bytes bad = capture;
    bad[15] ^= 1;
    assert(!sportstech::decodeStatus(bad.data(), bad.size(), status));
    bad = capture;
    bad.back() = 0;
    assert(!sportstech::decodeStatus(bad.data(), bad.size(), status));
    assert(!sportstech::decodeStatus(capture.data(), 4, status));
    assert(!sportstech::decodeStatus(nullptr, 17, status));
    const Bytes startAck = sportstech::frame(0x51, {0x01});
    assert(!sportstech::decodeStatus(startAck.data(), startAck.size(), status));
    const Bytes unknown = sportstech::frame(0x51, {0x07});
    assert(!sportstech::decodeStatus(unknown.data(), unknown.size(), status));
}

void sourceTests() {
    auto created = createProtocol("sportstech-f15");
    assert(created && created->profile() == MachineProfile::Treadmill);
    const auto* info = findProtocol("sportstech-f15");
    assert(info && info->controllable && info->priority == 0);
    ble::Advertisement advertisement;
    advertisement.name = "F15";
    advertisement.services.push_back(sportstech::kService);
    assert(!info->matches(advertisement));

    SportstechF15Source source;
    const auto plan = source.plan();
    assert(plan.subscriptionCount == 1 && plan.subscriptions[0].service == sportstech::kService &&
           plan.subscriptions[0].characteristic == sportstech::kNotify);
    assert(!source.onConnected(0, 0));
    source.reset();

    std::uint32_t now = 100;
    assert(source.onConnected(1, now));
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.payload() == sportstech::pollFrame() && !write.withResponse &&
           write.characteristic == sportstech::kWrite);
    assert(!source.isTrainer());
    assert(!source.control({Control::Speed, 5}, now));
    source.tick(now + 999);
    assert(!source.takeWrite(write));
    source.tick(now + 1000);
    assert(source.takeWrite(write) && write.payload() == sportstech::pollFrame());

    // Stopped: speed 0 reported, belt is never started.
    now += 1001;
    notify(source, Bytes({0x02, 0x51, 0x00, 0x51, 0x03}), now);
    assert(source.mode() == Mode::Stopped && source.isTrainer());
    assert(source.sample().speed.valid && near(source.sample().speed.value, 0));
    assert(!source.sample().distance.valid && !source.sample().heartRate.valid);
    assert(!source.control({Control::Speed, 5}, now));
    assert(!source.control({Control::Incline, 2}, now));
    assert(!source.control({Control::Speed, 0}, now));
    assert(!source.takeWrite(write));

    // Running: every decoded value is forwarded.
    notify(source, running(50, 20, 120), ++now);
    const auto& sample = source.sample();
    assert(source.mode() == Mode::Running);
    assert(near(sample.speed.value, 5.0F) && near(sample.incline.value, 2.0F) && near(sample.elapsed.value, 16) &&
           near(sample.distance.value, 37) && near(sample.totalEnergy.value, 1.4F) &&
           near(sample.heartRate.value, 120));
    const auto capabilities = source.capabilities();
    for (Metric metric : {Metric::Speed, Metric::Incline, Metric::ElapsedTime, Metric::Distance,
                          Metric::TotalEnergy, Metric::HeartRate})
        assert(capabilities.supports(metric));
    assert(capabilities.supports(Control::Speed) && capabilities.supports(Control::Incline));
    notify(source, running(50, 20, 0), ++now);
    assert(!source.sample().heartRate.valid);

    // Bounds follow the upstream number entities.
    assert(!source.control({Control::Speed, 0.5F}, now));
    assert(!source.control({Control::Speed, 12.5F}, now));
    assert(!source.control({Control::Incline, 16}, now));
    assert(!source.control({Control::Incline, -1}, now));
    assert(!source.control({Control::Speed, std::numeric_limits<float>::quiet_NaN()}, now));
    assert(!source.takeWrite(write));

    // Speed and incline share one frame; the partner keeps the pending target.
    assert(source.control({Control::Speed, 8}, now));
    assert(source.takeWrite(write) && write.payload() == sportstech::speedInclineFrame(80, 2));
    assert(source.control({Control::Incline, 4.4F}, now));
    assert(source.takeWrite(write) && write.payload() == sportstech::speedInclineFrame(80, 4));
    notify(source, running(65, 0), ++now);  // still ramping
    assert(source.control({Control::Incline, 5}, now));
    assert(source.takeWrite(write) && write.payload() == sportstech::speedInclineFrame(80, 5));
    notify(source, running(80, 50), ++now);  // targets reached
    notify(source, running(90, 50), ++now);  // console speed change is followed
    assert(source.control({Control::Incline, 6}, now));
    assert(source.takeWrite(write) && write.payload() == sportstech::speedInclineFrame(90, 6));

    // Stale telemetry disables controls.
    assert(!source.control({Control::Speed, 7}, now + 2001));

    // Speed 0 stops the belt.
    assert(source.control({Control::Speed, 0}, now));
    assert(source.takeWrite(write) && write.payload() == sportstech::stopFrame());

    source.reset();
    assert(!source.hasData() && !source.isTrainer() && source.mode() == Mode::Stopped);
}
}  // namespace

void runSportstechTests() {
    codecTests();
    sourceTests();
}
