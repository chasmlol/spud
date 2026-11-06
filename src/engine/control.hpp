#pragma once

// --control <port>: a localhost-only TCP session speaking newline-delimited
// JSON ({"id", "line"} -> {"id", "ok", "result" | "error"}). All socket IO
// happens on the server's own thread; the main loop takes requests at a fixed
// point in the frame and queues responses, which are sent in request order.

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spud::engine {

class ControlServer {
public:
    struct Request {
        uint64_t client = 0;
        std::string id;    // raw JSON value of "id" ("null" if absent)
        std::string line;  // the command line
        std::string error; // non-empty: malformed request, answer with this error
    };

    explicit ControlServer(uint16_t port); // throws if the port cannot be bound
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    uint16_t port() const { return m_port; }
    // Main thread: requests received since the last call, in order.
    std::vector<Request> poll();
    // Main thread: queue a response line (without the newline) for `client`.
    void respond(uint64_t client, std::string json);

    static std::string ok(const std::string& id, const std::string& result_json = {});
    static std::string error(const std::string& id, const std::string& message);

private:
    struct Client;
    void thread_main();

    uint16_t m_port;
    uintptr_t m_listen = ~uintptr_t(0);
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    std::vector<Request> m_inbound;
    std::deque<std::pair<uint64_t, std::string>> m_outbound;
};

std::string json_escape(const std::string& s);

} // namespace spud::engine
