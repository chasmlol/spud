#include "engine/sidebyside.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "engine/control.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <format>
#include <sstream>

namespace spud::engine {

namespace {

FILE* open_jsonl(const std::filesystem::path& p) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"ab") != 0) {
        throw Error("side by side: cannot write " + p.string());
    }
    setvbuf(f, nullptr, _IOFBF, 1 << 16);
    return f;
}

} // namespace

SideBySide::SideBySide(uint16_t port, std::filesystem::path dir, int64_t qpc0) : m_dir(std::move(dir)), m_qpc0(qpc0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    m_tick = 1.0 / double(f.QuadPart);
    if (!m_qpc0) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        m_qpc0 = c.QuadPart;
    }
    std::filesystem::create_directories(m_dir);
    m_frames = open_jsonl(m_dir / "spud.frames.jsonl");
    m_world = open_jsonl(m_dir / "spud.world.jsonl");
    m_events = open_jsonl(m_dir / "spud.events.jsonl");
    m_log = open_jsonl(m_dir / "spud.log.jsonl");

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw Error("side by side: WSAStartup failed");
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 1) != 0) {
        throw Error(std::format("side by side: cannot listen on 127.0.0.1:{}", port));
    }
    m_listen = static_cast<uintptr_t>(s);
    m_thread = std::thread([this] { thread_main(); });
    log::info("side by side: waiting for the game on 127.0.0.1:{}, telemetry in {}", port, m_dir.string());
}

SideBySide::~SideBySide() {
    m_stop = true;
    closesocket(static_cast<SOCKET>(m_listen));
    if (const uintptr_t c = m_client.exchange(~uintptr_t(0)); c != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(c));
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    WSACleanup();
    for (FILE* f : {m_frames, m_world, m_events, m_log}) {
        if (f) {
            std::fclose(f);
        }
    }
}

double SideBySide::now() const {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart - m_qpc0) * m_tick;
}

void SideBySide::thread_main() {
    while (!m_stop) {
        const SOCKET c = accept(static_cast<SOCKET>(m_listen), nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            continue;
        }
        BOOL nodelay = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        m_client = static_cast<uintptr_t>(c);
        m_connected = true;
        log::info("side by side: the game is connected");
        std::string buffer;
        char chunk[4096];
        for (;;) {
            const int n = recv(c, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                break;
            }
            buffer.append(chunk, size_t(n));
            size_t start = 0;
            for (size_t nl; (nl = buffer.find('\n', start)) != std::string::npos; start = nl + 1) {
                parse(buffer.substr(start, nl - start));
            }
            buffer.erase(0, start);
        }
        m_connected = false;
        if (m_client.exchange(~uintptr_t(0)) != ~uintptr_t(0)) {
            closesocket(c);
        }
        std::lock_guard lock(m_mutex);
        m_pose.valid = false;
        m_input.push_back({.kind = MirrorInput::Kind::release_all}); // nothing stays held for a game that left
        log::info("side by side: the game left");
    }
}

// One line of the wire protocol.
void SideBySide::parse(const std::string& line) {
    std::istringstream in(line);
    std::string op;
    in >> op;
    if (op.empty()) {
        return;
    }
    double t = 0;
    in >> t;
    using K = MirrorInput::Kind;
    MirrorInput e;
    e.t = t;
    if (op == "P") {
        MirrorPose p;
        p.t = t;
        int interior = 0, settled = 0;
        in >> p.position.x >> p.position.y >> p.position.z >> p.velocity.x >> p.velocity.y >> p.velocity.z >> p.heading >>
            p.pitch >> p.hour >> std::hex >> p.weather >> p.cell >> std::dec >> interior >> settled >> p.view >> p.world;
        p.valid = !in.fail();
        p.interior = interior != 0;
        p.settled = settled != 0;
        std::lock_guard lock(m_mutex);
        m_pose = std::move(p);
        return;
    }
    if (op == "K" || op == "B") {
        int down = 0;
        in >> e.code >> down;
        e.kind = op == "K" ? K::key : K::mouse_button;
        e.down = down != 0;
    } else if (op == "M") {
        e.kind = K::mouse_move;
        in >> e.x >> e.y;
    } else if (op == "W") {
        e.kind = K::wheel;
        in >> e.x;
    } else if (op == "C") {
        e.kind = K::text;
        in >> e.code;
    } else if (op == "R") {
        e.kind = K::release_all;
    } else if (op == "MARK") {
        e.kind = K::mark;
        in >> e.code;
    } else if (op == "SYNC") {
        e.kind = K::resync;
    } else {
        return;
    }
    std::lock_guard lock(m_mutex);
    m_input.push_back(e);
}

std::vector<MirrorInput> SideBySide::take_input() {
    std::lock_guard lock(m_mutex);
    std::vector<MirrorInput> out(m_input.begin(), m_input.end());
    m_input.clear();
    return out;
}

MirrorPose SideBySide::pose() {
    std::lock_guard lock(m_mutex);
    return m_pose;
}

void SideBySide::write(FILE* f, double t, std::string_view fields) {
    std::fprintf(f, "{\"t\":%.4f,%.*s}\n", t, int(fields.size()), fields.data());
}

void SideBySide::frame(std::string_view fields) { write(m_frames, now(), fields); }

void SideBySide::world(std::string_view fields) { write(m_world, now(), fields); }

void SideBySide::event(std::string_view type, std::string_view fields) {
    write(m_events, now(), std::format("\"e\":\"{}\"{}{}", type, fields.empty() ? "" : ",", fields));
    std::fflush(m_events);
}

void SideBySide::log_line(int level, std::string_view message) {
    std::lock_guard lock(m_log_mutex);
    write(m_log, now(), std::format("\"lvl\":{},\"msg\":\"{}\"", level, json_escape(std::string(message))));
    std::fflush(m_log);
}

void SideBySide::flush() {
    std::fflush(m_frames);
    std::fflush(m_world);
}

} // namespace spud::engine
