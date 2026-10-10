#pragma once

// Thread boundary between the async web server task and the main loop.
// HTTP/WebSocket handlers call submit()/result()/snapshot() only; the main
// loop calls process() and publish(). BridgeCore is never touched from the
// web task. Pure C++ (host tested).

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ble/bounded_queue.hpp"
#include "bridge_commands.h"

class WebUi;

struct ApiResponse {
    int status = 200;
    std::string body;  // JSON
};

// Parses a POST body for `endpoint` (e.g. "control", "pair") into commands.
// Returns false with a client error message on invalid input.
bool parseApiRequest(const std::string& endpoint, const std::string& body, std::vector<Command>& commands,
                     std::string& error);

class ApiExchange {
public:
    static constexpr std::size_t kMaxBodySize = 1024;
    static constexpr std::size_t kQueueCapacity = 8;
    static constexpr std::size_t kResultCapacity = 16;

    enum class Snapshot : std::uint8_t { Status, Scan, Config, Count };

    struct Job {
        std::uint32_t id = 0;
        std::vector<Command> commands;
    };

    // --- web task ---
    // Validates and enqueues; 202 {"id":n} on success, 400/413/503 otherwise.
    ApiResponse submit(const std::string& endpoint, const std::string& body);
    // 200 with the result, 202 while pending, 404 for unknown/expired ids.
    ApiResponse result(std::uint32_t id) const;
    std::string snapshot(Snapshot kind) const;

    // --- main loop ---
    // Executes up to maxJobs queued jobs; returns the result JSON messages
    // (also stored for result()) so they can be pushed over the WebSocket.
    std::vector<std::string> process(WebUi& ui, std::size_t maxJobs = 2);
    void publish(Snapshot kind, const std::string& json);

    std::uint32_t droppedRequests() const { return queue_.dropped(); }

private:
    struct Stored {
        std::uint32_t id = 0;
        bool done = false;
        std::string json;
    };
    void store(std::uint32_t id, bool done, const std::string& json);

    smartfitness::BoundedQueue<Job, kQueueCapacity> queue_;
    mutable std::mutex mutex_;
    std::uint32_t nextId_ = 1;
    std::array<Stored, kResultCapacity> results_{};
    std::size_t resultHead_ = 0;
    std::array<std::string, static_cast<std::size_t>(Snapshot::Count)> snapshots_{};
};
