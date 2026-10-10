// Host tests: existing repository uses assert, not doctest.
// Frames follow pinned qdomyos-zwift parsers/commands at
// ea70d7ff64bc8bf508fc6401fd056211e05311b3 (see port attribution).
#include "protocols/ifit_bowflex.hpp"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>

using namespace smartfitness;
using namespace smartfitness::protocols;
using namespace smartfitness::protocols::ifit_bowflex;

namespace {
using Bytes = ble::Bytes;
std::vector<Bytes> frames(const char* text) {
    std::istringstream input(text);
    std::string token;
    std::vector<Bytes> result;
    while (input >> token) {
        assert(token.size() % 2 == 0);
        Bytes frame;
        for (std::size_t i = 0; i < token.size(); i += 2)
            frame.push_back(static_cast<std::uint8_t>(std::stoul(token.substr(i,2),nullptr,16)));
        assert(!frame.empty() && frame.size() <= ble::kMaxWriteLength);
        result.push_back(frame);
    }
    return result;
}
void drain(SourceProtocol& source) {
    ble::GattWrite write;
    while (source.takeWrite(write)) {}
}
bool special(const Model& model) {
    return std::strcmp(model.name,"nordictrack_se7i") == 0 ||
           std::strcmp(model.name,"nordictrack_elliptical_s700") == 0;
}
std::uint32_t initialize(IfitBowflexSource& source, const Model& model, std::uint32_t start = 100) {
    assert(source.onConnected(0xff,start));
    const auto expected = frames(model.init);
    const auto spacing = model.kind == Kind::BowflexT216 ? 300U : 400U;
    auto now = start;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ble::GattWrite write;
        assert(source.takeWrite(write));
        assert(write.payload() == expected[i]);
        assert(write.withResponse);
        assert(!source.takeWrite(write));
        assert(!source.initialized());
        source.tick(now + spacing - 1);
        assert(!source.takeWrite(write));
        if (special(model) && i >= 13 && (i - 12) % 3 == 0) {
            const Bytes ack{0xff,0x05};
            source.onNotify(0,ack.data(),ack.size(),now);
        }
        now += spacing;
        source.tick(now);
    }
    assert(source.initialized() && !source.failed());
    return now;
}
Bytes status(std::uint8_t tag = 0x2e) {
    Bytes data(20,0);
    data[1] = 0x12; data[2] = 1; data[3] = 4; data[4] = 2; data[5] = tag;
    return data;
}
void notify(IfitBowflexSource& source, const Bytes& data, std::uint32_t now, std::size_t index = 0) {
    source.onNotify(index,data.data(),data.size(),now);
}

void testAllProfiles() {
    assert(modelCount() >= 90);
    for (std::size_t i = 0; i < modelCount(); ++i) {
        const auto& model = modelAt(i);
        assert(findModel(model.name) == &model);
        if (model.kind == Kind::Schwinn170) continue;
        IfitBowflexSource source(model);
        const auto now = initialize(source,model,0xfffff000U);
        const auto poll = frames(model.poll);
        for (std::size_t j = 0; j < poll.size(); ++j) {
            source.tick(now + static_cast<std::uint32_t>((j + 1)*200));
            ble::GattWrite write;
            assert(source.takeWrite(write));
            assert(write.payload() == poll[j]);
        }
        for (std::size_t n = 0; n < 40; ++n) {
            Bytes junk(n,0xff);
            source.onNotify(0,junk.data(),junk.size(),now);
            source.onNotify(0,nullptr,n,now);
            source.onNotify(100,junk.data(),junk.size(),now);
        }
        assert(!source.hasData());
        assert(!source.control({Control::Power,100},now));
        assert(!source.control({Control::Resistance,-1},now));
        assert(!source.control({Control::Incline,std::numeric_limits<float>::quiet_NaN()},now));
        if (source.capabilities().supports(Control::Resistance)) {
            assert(source.control({Control::Resistance,0},now));
            ble::GattWrite write;
            std::vector<Bytes> group;
            while (source.takeWrite(write)) group.push_back(write.payload());
            assert(group.size() >= 2 && group.front()[0] == 0xfe && group.back()[0] == 0xff);
            if (!source.control({Control::Resistance,100},now)) {
                std::cerr << "Missing maximum resistance control: " << model.name << " (" << model.maximumResistance << ")\n";
                assert(false);
            }
            drain(source);
        }
        source.onWriteFailed(now);
        assert(source.failed() && source.capabilities().controls == 0);
        assert(!source.control({Control::Resistance,50},now));
        source.reset();
        assert(!source.hasData() && !source.failed() && !source.pendingWrites());
    }
}
void testBike() {
    const auto& model = *findModel("proformbike");
    IfitBowflexSource source(model);
    const auto now = initialize(source,model);
    auto data = status();
    data[11] = 11; data[12] = 150; data[18] = 80;
    notify(source,data,now);
    assert(source.sample().cadence.value == 80 && source.sample().power.value == 150);
    assert(source.sample().resistance.value == 5);
    assert(!source.sample().speed.valid && !source.sample().heartRate.valid);
    assert(source.control({Control::Resistance,0},now));
    ble::GattWrite write;
    assert(source.takeWrite(write) && write.payload() == Bytes({0xfe,0x02,0x0d,0x02}));
    assert(source.takeWrite(write));
    assert(write.payload() == Bytes({0xff,0x0d,0x02,0x04,0x02,0x09,0x07,0x09,0x02,0x01,
                                    0x04,0x32,0x02,0,0x4b,0,0,0,0,0}));
    data[12] = 0xff; data[13] = 0xff;
    notify(source,data,now+1);
    assert(source.lastDataMs() == now);
    for (int i = 0; i < 8; ++i) assert(source.control({Control::Resistance,50},now));
    assert(!source.control({Control::Resistance,50},now) && source.pendingWrites() == 16);

    IfitBowflexSource studioSource(*findModel("proform_studio"));
    const auto sn = initialize(studioSource,*findModel("proform_studio"));
    assert(studioSource.control({Control::Resistance,100},sn));
    assert(studioSource.takeWrite(write)); assert(studioSource.takeWrite(write));
    assert(write.data[18] == 32);
    assert(studioSource.takeWrite(write) && write.data[5] == 0x65);
}
void testTreadmillSafety() {
    const auto& model = *findModel("proformtreadmill");
    IfitBowflexSource source(model);
    auto now = initialize(source,model);
    assert(!source.control({Control::Speed,10},now));
    auto data = status();
    notify(source,data,now);
    assert(!source.control({Control::Speed,10},now));
    data[10] = 0xe8; data[11] = 3; data[12] = 0xc8; data[13] = 0xff;
    notify(source,data,now);
    assert(source.sample().speed.value == 10);
    assert(source.sample().incline.value == -0.56F);
    assert(!source.sample().power.valid);
    assert(source.control({Control::Speed,12.5F},now));
    ble::GattWrite write;
    assert(source.takeWrite(write)); assert(source.takeWrite(write));
    assert(write.data[11] == 0xe2 && write.data[12] == 4 && write.data[14] == 0xf7);
    assert(!source.control({Control::Speed,23},now));
    assert(!source.control({Control::Speed,12},now+3001));
    assert(!source.control({Control::Incline,16},now));
}
void testEllipticals() {
    IfitBowflexSource stride(Kind::FreeStride);
    auto now = initialize(stride,*findModel("proformelliptical"));
    auto data = status(); data[11] = 17; data[12] = 200; data[18] = 60;
    notify(stride,data,now);
    assert(stride.sample().resistance.value == 10 && stride.sample().power.value == 200);
    assert(!stride.isTrainer());
    data[0] = 1; data[14] = 0xe8; data[15] = 3;
    notify(stride,data,now);
    assert(stride.sample().speed.value == 10);

    IfitBowflexSource trainer(Kind::EllipticalTrainer);
    now = initialize(trainer,*findModel("proformellipticaltrainer"));
    data = status(); data[13] = 3; data[14] = 100;
    notify(trainer,data,now);
    assert(trainer.sample().resistance.value == 2);
    assert(std::fabs(trainer.sample().speed.value-16.0934F)<0.001F);
    assert(!trainer.sample().power.valid);

    const auto& se = *findModel("nordictrack_se7i");
    IfitBowflexSource source(se);
    now = initialize(source,se);
    data = status(0x30); data[10] = 0xf4; data[11] = 1; data[12] = 0xc5; data[13] = 1; data[14] = 200;
    notify(source,data,now);
    assert(source.sample().resistance.value == 1 && source.sample().incline.value == 5);
    assert(source.sample().power.value == 200);
    assert(source.control({Control::Resistance,0},now));
    ble::GattWrite write;
    assert(source.takeWrite(write)); assert(source.takeWrite(write));
    assert(write.data[11] == 0xc5 && write.data[12] == 1 && write.data[14] == 0);
    data = Bytes(20,0); data[0] = 1; data[1] = 0x12; data[2] = 75; data[4] = 0x46; data[12] = 0xe8; data[13] = 3;
    notify(source,data,now);
    assert(source.sample().speed.value == 10 && source.sample().cadence.value == 75);

    IfitBowflexSource timeout(se);
    assert(timeout.onConnected(1,0));
    for (unsigned i = 0; i < 16; ++i) { drain(timeout); timeout.tick((i+1)*400); }
    drain(timeout); timeout.tick(10000);
    assert(timeout.failed());
}
void testBowflexSchwinn() {
    IfitBowflexSource bow(Kind::BowflexT216);
    const auto now = initialize(bow,*findModel("bowflex-t216"));
    assert(bow.plan().subscriptionCount == 5 && !bow.isTrainer());
    Bytes data(20,0); data[4] = 8; data[6] = 0xe8; data[7] = 3;
    notify(bow,data,now,0); assert(!bow.hasData());
    notify(bow,data,now,2);
    assert(std::fabs(bow.sample().speed.value-16.0934F)<0.001F && bow.sample().incline.value == 8);
    assert(!bow.control({Control::Speed,10},now));

    IfitBowflexSource unknown(Kind::Schwinn170);
    assert(!unknown.onConnected(1,0));
    ble::ConnectPlan plan;
    plan.subscribe(ble::Uuid::from16(0xff00),ble::Uuid::from16(0xff01));
    const auto& model = *findModel("schwinn-170");
    IfitBowflexSource schwinn(model,plan,ble::Uuid::from16(0xff00),ble::Uuid::from16(0xff02));
    auto sn = initialize(schwinn,model);
    data = Bytes(14,0); data[3] = 0xe8; data[4] = 3; data[6] = 0x98; data[7] = 0x3a;
    notify(schwinn,data,sn);
    assert(schwinn.sample().speed.value == 10 && schwinn.sample().power.value == 150);
    data = Bytes(17,0); data[4] = 0xff; data[5] = 0xff; notify(schwinn,data,sn);
    data[4] = 0; data[5] = 0; notify(schwinn,data,sn+1000);
    assert(schwinn.sample().cadence.value == 60);
    schwinn.tick(sn+4000);
    assert(schwinn.sample().cadence.value == 0);
    assert(!schwinn.isTrainer());
}
void testMatchers() {
    const char* names[] = {"I_EB123","I_TL123","i_fs1","i_el1","I_VE1","BOWFLEX T216","SCHWINN 170/270 abc"};
    for (std::size_t i = 0; i < descriptorCount(); ++i) {
        const auto& desc = descriptorAt(i);
        ble::Advertisement ad; ad.name = names[i];
        assert(desc.matches(ad) && desc.priority == 85 && !desc.advertisementOnly);
        assert(std::strlen(desc.id) <= 16);
        auto source = desc.create();
        assert(std::strcmp(source->id(),desc.id) == 0);
        for (const char* excluded : {"Peloton","DFC","VeloCore","NordicTrack WiFi","ProForm","I_RW123"}) {
            ad.name = excluded; assert(!desc.matches(ad));
        }
    }
}
void testNoInterleavedControl() {
    IfitBowflexSource source(Kind::Bike);
    const auto now = initialize(source,*findModel("proformbike"));
    source.tick(now+200); drain(source);
    assert(!source.control({Control::Resistance,50},now+200));
    source.tick(now+400); drain(source);
    assert(!source.control({Control::Resistance,50},now+400));
    source.tick(now+600); drain(source);
    assert(source.control({Control::Resistance,50},now+600));
}
void testVerifiedNestedInitBranches() {
    // proformbike.cpp:3069-3146, independently compiler-executed upstream
    // btinit() branches, not expectations copied from the exported table.
    const auto pfe = frames(findModel("proform_bike_PFEVEX71316_1")->init);
    const auto tdf = frames(findModel("proform_tdf_10")->init);
    assert(pfe.size() == 16 && tdf.size() == 16);
    assert(pfe[3] == frames("ff08020402040804808c00000000000000000000")[0]);
    assert(tdf[3] == pfe[3]);
    assert(pfe[13] == frames("0012020402280828900400c158fd9031d07528c1")[0]);
    assert(tdf[13] == frames("0012020402280828900400400cca9e6438f6daa8")[0]);
    // VR21 deliberately repeats the discovery prefix in the pinned source:
    // proformbike.cpp:3715 onward; removing duplicates breaks the transcript.
    const auto vr = frames(findModel("nordictrack_vr21")->init);
    assert(vr.size() == 44 && vr[0] == vr[13] && vr[1] == vr[14]);
}
}
void runIfitBowflexTests() {
    testAllProfiles();
    testBike();
    testTreadmillSafety();
    testEllipticals();
    testBowflexSchwinn();
    testMatchers();
    testNoInterleavedControl();
    testVerifiedNestedInitBranches();
    std::cout << "iFit/Bowflex host tests passed (" << modelCount() << " profiles)\n";
}
