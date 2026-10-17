// Standalone focused test, using the same g++/assert infrastructure as host tests:
// g++ -std=c++17 -Wall -Wextra -Werror -pthread -DDEBUG_WEB -DARDUINO
//   -Iinclude/core -Itests/host/debug_serial_fakes tests/host/test_debug_serial.cpp -o /tmp/smartfitness-debug-test
#include "../../src/platform/debug_serial.cpp"

#include <atomic>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

namespace {
void reset() {
    logFile.close();
    delete fsMutex;
    delete serialMutex;
    fsMutex = serialMutex = nullptr;
    workerTask = nullptr;
    head = queued = 0;
    std::memset(queueToSerial, 0, sizeof(queueToSerial));
    absoluteStart = absoluteEnd = lost = 0;
    writable = started = serialOnly = false;
    fake::isr = false;
    fake::mounted = true;
    fake::mutexAllocations = fake::mutexAllocationFailAt = 0;
    fake::allMutexAllocationsFail = fake::taskAllocationFails = false;
    fake::openFails = fake::readFails = false;
    fake::writeLimit = SIZE_MAX;
    fake::otherFiles = 0;
    fake::formats = fake::opens = fake::flushes = fake::mounts = 0;
    fake::serialCalls = fake::fsCalls = fake::mutexCalls = 0;
    fake::maxSize = 0;
    fake::currentTask = nullptr;
    fake::activeVprintf = nullptr;
    fake::logLevel = -1;
    fake::emitNestedLog = false;
    fake::file.clear();
    fake::partition.assign(fake::partition.size(), 0xff);
    Serial.output.clear();
}

void pump() {
    fake::workerIterations = 1;
    try { fake::task(nullptr); } catch (const fake::StopWorker&) {}
}

std::size_t writeText(const std::string& text) {
    return DBG_SERIAL.write(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

struct Snapshot {
    std::string text;
    std::uint64_t end = 0;
    unsigned calls = 0;
    unsigned failAt = ~0u;
    bool enqueue = false;
};

bool sink(const char* data, std::size_t size, std::uint64_t end, void* context) {
    auto& snapshot = *static_cast<Snapshot*>(context);
    if (snapshot.calls++ == snapshot.failAt) return false;
    if (!data) {
        assert(size == 0 && snapshot.calls == 1);
        snapshot.end = end;
        if (snapshot.enqueue) {
            char buffer[8];
            assert(debugweb::readChunk(0, buffer, sizeof(buffer)).size == 0);
            assert(writeText("queued during download") == 22);
        }
    } else {
        assert(snapshot.calls > 1 && size <= 1024 && end == snapshot.end);
        snapshot.text.append(data, size);
    }
    return true;
}

void testMountAndBoot() {
    reset();
    fake::file = "previous boot";
    debugweb::begin();
    assert(fake::file.empty() && fake::formats == 0 && fake::opens == 1);
    debugweb::begin();
    assert(fake::opens == 1);
    const auto flushesBefore = fake::flushes;
    pump();
    assert(fake::flushes == flushesBefore);  // No filesystem activity when idle.

    reset();
    fake::mounted = false;
    debugweb::begin();
    assert(fake::formats == 1 && fake::mounts == 2 && writable);

    reset();
    fake::mounted = false;
    fake::partition.back() = 0;  // Check ALL of the partition, not just a header.
    fake::file = "preserve corrupted filesystem";
    debugweb::begin();
    assert(fake::formats == 0 && fake::file == "preserve corrupted filesystem");
    assert(Serial.output.find("NOT formatted") != std::string::npos);
    Snapshot snapshot;
    assert(!debugweb::streamLog(sink, &snapshot) && snapshot.calls == 0);
    writeText("Serial still works");
    pump();
    assert(Serial.output.find("Serial still works") != std::string::npos);

    reset();
    fake::otherFiles = LittleFS.totalBytes() - 100;
    debugweb::begin();
    assert(Serial.output.find("insufficient free") != std::string::npos);
}

void testBatchingReadsAndSnapshots() {
    reset();
    debugweb::begin();
    for (char byte : std::string("hello")) DBG_SERIAL.write(static_cast<std::uint8_t>(byte));
    assert(fake::file.empty() && Serial.output.empty());
    pump();
    assert(fake::file == "hello" && Serial.output == fake::file && fake::opens == 1);
    char data[8]{};
    const auto chunk = debugweb::readChunk(1, data, 3);
    assert(chunk.start == 1 && chunk.end == 4 && chunk.size == 3 && !chunk.reset);
    assert(std::string(data, chunk.size) == "ell");
    writeText(" world");
    pump();
    assert(fake::file == "hello world");  // Reading did not move the append cursor.
    const auto future = debugweb::readChunk(UINT64_MAX, data, sizeof(data));
    assert(future.start == 11 && future.end == 11 && !future.size);

    Snapshot snapshot;
    snapshot.enqueue = true;
    assert(debugweb::streamLog(sink, &snapshot));
    assert(snapshot.end == 11 && snapshot.text == "hello world");
    assert(fake::file == "hello world");  // Producers never wait on the FS mutex.
    pump();
    assert(fake::file == "hello worldqueued during download");

    for (unsigned failAt : {0u, 1u}) {
        Snapshot failed;
        failed.failAt = failAt;
        assert(!debugweb::streamLog(sink, &failed));
        assert(debugweb::readChunk(0, data, sizeof(data)).size == sizeof(data));
    }
    fake::readFails = true;
    Snapshot failedRead;
    assert(!debugweb::streamLog(sink, &failedRead));
    fake::readFails = false;
    assert(debugweb::readChunk(0, data, sizeof(data)).size == sizeof(data));
}

void testLossAndIsr() {
    reset();
    debugweb::begin();
    assert(writeText(std::string(kQueueBytes + 50, 'x')) == kQueueBytes);
    pump();
    assert(fake::file.find("LOST OUTPUT: 50") != std::string::npos);
    assert(fake::file == Serial.output);

    reset();
    debugweb::begin();
    const auto serialBefore = fake::serialCalls;
    const auto fsBefore = fake::fsCalls;
    const auto mutexBefore = fake::mutexCalls;
    fake::isr = true;
    assert(DBG_SERIAL.printf("%s", "not safe in ISR") == 0);
    assert(DBG_SERIAL.write(static_cast<std::uint8_t>('x')) == 0);
    char data[8];
    assert(debugweb::readChunk(0, data, sizeof(data)).size == 0);
    Snapshot snapshot;
    assert(!debugweb::streamLog(sink, &snapshot));
    debugweb::begin();
    assert(fake::serialCalls == serialBefore && fake::fsCalls == fsBefore);
    assert(fake::mutexCalls == mutexBefore);
    fake::isr = false;
    pump();
    assert(fake::file.find("LOST OUTPUT: 5") != std::string::npos);

    reset();
    debugweb::begin();
    assert(DBG_SERIAL.printf("value=%d", 42) == 8);
    pump();
    assert(fake::file == "value=42");
    const std::string longMessage(2000, 'z');
    assert(DBG_SERIAL.printf("%s", longMessage.c_str()) == 1023);
    pump();
    assert(fake::file.find("LOST OUTPUT: 977") != std::string::npos);
}

void testRotationAndFailure() {
    reset();
    debugweb::begin();
    const std::string block(kQueueBytes, 'r');
    for (unsigned i = 0; i < 65; ++i) {
        assert(writeText(block) == block.size());
        pump();
        assert(fake::file.size() <= debugweb::kMaxLogBytes);
    }
    assert(fake::maxSize == debugweb::kMaxLogBytes);
    assert(absoluteStart == debugweb::kMaxLogBytes);
    assert(absoluteEnd == 65 * kQueueBytes && fake::file == block);
    char data[8];
    const auto stale = debugweb::readChunk(0, data, sizeof(data));
    assert(stale.reset && stale.start == absoluteStart && stale.end == absoluteStart + sizeof(data));
    Snapshot snapshot;
    assert(debugweb::streamLog(sink, &snapshot));
    assert(snapshot.end == absoluteEnd && snapshot.text == block && snapshot.calls == 9);

    fake::writeLimit = 2;
    writeText("lost to flash");
    pump();
    assert(Serial.output.find("persistent output lost") != std::string::npos);
    assert(!writable && absoluteEnd == 65 * kQueueBytes + 2);

    reset();
    fake::openFails = true;
    debugweb::begin();
    assert(Serial.output.find("cannot open") != std::string::npos);
    Snapshot missing;
    assert(!debugweb::streamLog(sink, &missing));
}

void testInitializationFallback() {
    // Failure of either mutex, both mutexes, or worker allocation must preserve
    // the original task-context Serial console without accessing the filesystem.
    for (unsigned failure = 1; failure <= 4; ++failure) {
        reset();
        fake::mutexAllocationFailAt = failure <= 2 ? failure : 0;
        fake::allMutexAllocationsFail = failure == 3;
        fake::taskAllocationFails = failure == 4;
        debugweb::begin();
        assert(serialOnly && !workerTask && !logFile);
        assert(Serial.output.find("direct Serial fallback") != std::string::npos);
        const auto fsBefore = fake::fsCalls;
        const auto mountsBefore = fake::mounts;
        const auto opensBefore = fake::opens;
        assert(writeText("console remains available") == 25);
        assert(Serial.output.find("console remains available") != std::string::npos);
        fake::isr = true;
        assert(DBG_SERIAL.printf("%d", 123) == 0);
        fake::isr = false;
        DBG_SERIAL.println("task output");
        assert(Serial.output.find("LOST OUTPUT: 1") != std::string::npos);
        assert(Serial.output.find("task output") != std::string::npos);
        Snapshot unavailable;
        assert(!debugweb::streamLog(sink, &unavailable));
        assert(fake::fsCalls == fsBefore && fake::mounts == mountsBefore && fake::opens == opensBefore);
    }
}

void testEspLogForwarding() {
    reset();
    debugweb::begin();
    assert(fake::activeVprintf != nullptr && fake::logLevel == ESP_LOG_INFO);

    fake::emit("\033[0;32m[IDF] ready\033[0m\n");
    fake::emitNestedLog = true;
    fake::emit("[IDF] outer\n");
    writeText("application output\n");
    fake::emit("[IDF] after application\n");
    fake::currentTask = workerTask;
    fake::emit("[IDF] logger task\n");
    fake::currentTask = nullptr;
    pump();

    assert(fake::file == "[IDF] ready\n[IDF] outer\napplication output\n[IDF] after application\n");
    assert(fake::file.find("nested logger message") == std::string::npos);
    assert(fake::file.find("[IDF] logger task") == std::string::npos);
    assert(Serial.output.find("\033[0;32m[IDF] ready\033[0m\n") != std::string::npos);
    assert(Serial.output.find("[IDF] ready\n") == std::string::npos);
}

void testConcurrentProducers() {
    reset();
    debugweb::begin();
    // Leave the ring head near its end so concurrent writes also wrap around.
    assert(writeText(std::string(6000, 'p')) == 6000);
    pump();
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < 4; ++producer) {
        producers.emplace_back([producer, &start] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned record = 0; record < 128; ++record) {
                assert(DBG_SERIAL.printf("[%u:%03u]\n", producer, record) == 8);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& producer : producers) producer.join();
    assert(queued == 4 * 128 * 8 && lost == 0);
    assert(fake::file.size() == 6000);  // No producer performed filesystem I/O.
    pump();
    assert(fake::file == Serial.output && fake::file.size() == 6000 + 4 * 128 * 8);
    std::istringstream records(fake::file.substr(6000));
    std::set<std::string> received;
    std::string record;
    while (std::getline(records, record)) assert(received.insert(record).second);
    assert(received.size() == 4 * 128);
    for (unsigned producer = 0; producer < 4; ++producer) {
        for (unsigned index = 0; index < 128; ++index) {
            char expected[16];
            std::snprintf(expected, sizeof(expected), "[%u:%03u]", producer, index);
            assert(received.count(expected) == 1);
        }
    }
}
}  // namespace

int main() {
    testMountAndBoot();
    testBatchingReadsAndSnapshots();
    testLossAndIsr();
    testRotationAndFailure();
    testInitializationFallback();
    testEspLogForwarding();
    testConcurrentProducers();
    reset();
    std::cout << "debug serial tests passed\n";
}
