#include "engine/control.hpp"

#include "core/error.hpp"
#include "core/log.hpp"

#include <format>
#include <memory>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace spud::engine {

namespace {

// Minimal parser for {"id": <value>, "line": "<string>"}; other keys are skipped.
struct JsonReader {
    const std::string& s;
    size_t i = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
            ++i;
        }
    }
    bool eat(char c) {
        ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    std::string string() {
        ws();
        if (i >= s.size() || s[i] != '"') {
            throw Error("expected a string");
        }
        ++i;
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\') {
                if (i >= s.size()) {
                    break;
                }
                const char e = s[i++];
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) {
                        throw Error("bad \\u escape");
                    }
                    const unsigned cp = static_cast<unsigned>(std::stoul(s.substr(i, 4), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (i >= s.size()) {
            throw Error("unterminated string");
        }
        ++i;
        return out;
    }
    // Returns the raw text of a scalar value (string, number, true/false/null).
    std::string raw_value() {
        ws();
        const size_t b = i;
        if (i < s.size() && s[i] == '"') {
            string();
            return s.substr(b, i - b);
        }
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\t') {
            if (s[i] == '{' || s[i] == '[') {
                throw Error("nested values are not supported");
            }
            ++i;
        }
        if (i == b) {
            throw Error("expected a value");
        }
        return s.substr(b, i - b);
    }
};

void parse_request(const std::string& text, ControlServer::Request& r) {
    JsonReader j{text};
    r.id = "null";
    if (!j.eat('{')) {
        throw Error("request must be a JSON object");
    }
    bool have_line = false;
    if (!j.eat('}')) {
        do {
            const std::string key = j.string();
            if (!j.eat(':')) {
                throw Error("expected ':'");
            }
            if (key == "line") {
                r.line = j.string();
                have_line = true;
            } else if (key == "id") {
                r.id = j.raw_value();
            } else {
                j.raw_value();
            }
        } while (j.eat(','));
        if (!j.eat('}')) {
            throw Error("expected '}'");
        }
    }
    if (!have_line) {
        throw Error("request has no \"line\"");
    }
}

} // namespace

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", int(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string ControlServer::ok(const std::string& id, const std::string& result_json) {
    if (result_json.empty()) {
        return std::format("{{\"id\": {}, \"ok\": true}}", id);
    }
    return std::format("{{\"id\": {}, \"ok\": true, \"result\": {}}}", id, result_json);
}

std::string ControlServer::error(const std::string& id, const std::string& message) {
    return std::format("{{\"id\": {}, \"ok\": false, \"error\": \"{}\"}}", id, json_escape(message));
}

#ifdef _WIN32

struct ControlServer::Client {
    uint64_t id;
    SOCKET socket;
    std::string in;
    std::string out;
};

ControlServer::ControlServer(uint16_t port) : m_port(port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw Error("control: WSAStartup failed");
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        throw Error("control: cannot create socket");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 4) != 0) {
        closesocket(s);
        WSACleanup();
        throw Error(std::format("control: cannot listen on 127.0.0.1:{}", port));
    }
    m_listen = static_cast<uintptr_t>(s);
    m_thread = std::thread([this] { thread_main(); });
    log::info("control: listening on 127.0.0.1:{}", port);
}

ControlServer::~ControlServer() {
    m_stop = true;
    m_thread.join();
    closesocket(static_cast<SOCKET>(m_listen));
    WSACleanup();
}

std::vector<ControlServer::Request> ControlServer::poll() {
    std::vector<Request> out;
    std::lock_guard lock(m_mutex);
    out.swap(m_inbound);
    return out;
}

void ControlServer::respond(uint64_t client, std::string json) {
    std::lock_guard lock(m_mutex);
    m_outbound.emplace_back(client, std::move(json));
}

void ControlServer::thread_main() {
    std::vector<std::unique_ptr<Client>> clients;
    uint64_t next_id = 1;
    const SOCKET listen_socket = static_cast<SOCKET>(m_listen);
    auto flush_outbound = [&] {
        std::deque<std::pair<uint64_t, std::string>> out;
        {
            std::lock_guard lock(m_mutex);
            out.swap(m_outbound);
        }
        for (auto& [cid, line] : out) {
            for (auto& c : clients) {
                if (c->id == cid) {
                    c->out += line;
                    c->out += '\n';
                }
            }
        }
        for (auto& c : clients) {
            while (!c->out.empty()) {
                const int sent = send(c->socket, c->out.data(), static_cast<int>(c->out.size()), 0);
                if (sent <= 0) {
                    break;
                }
                c->out.erase(0, static_cast<size_t>(sent));
            }
        }
    };
    while (!m_stop) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listen_socket, &read_set);
        for (auto& c : clients) {
            FD_SET(c->socket, &read_set);
        }
        timeval tv{0, 2000}; // 2 ms: bounds response latency, costs nothing
        const int ready = select(0, &read_set, nullptr, nullptr, &tv);
        if (ready > 0 && FD_ISSET(listen_socket, &read_set)) {
            SOCKET s = accept(listen_socket, nullptr, nullptr);
            if (s != INVALID_SOCKET) {
                BOOL nodelay = TRUE;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
                clients.push_back(std::make_unique<Client>(Client{next_id++, s, {}, {}}));
                log::info("control: client connected");
            }
        }
        for (size_t k = 0; ready > 0 && k < clients.size();) {
            Client& c = *clients[k];
            if (!FD_ISSET(c.socket, &read_set)) {
                ++k;
                continue;
            }
            char buf[4096];
            const int n = recv(c.socket, buf, sizeof(buf), 0);
            if (n <= 0) {
                closesocket(c.socket);
                clients.erase(clients.begin() + static_cast<ptrdiff_t>(k));
                log::info("control: client disconnected");
                continue;
            }
            c.in.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = c.in.find('\n')) != std::string::npos) {
                std::string text = c.in.substr(0, nl);
                c.in.erase(0, nl + 1);
                if (text.find_first_not_of(" \t\r") == std::string::npos) {
                    continue;
                }
                Request r;
                r.client = c.id;
                try {
                    parse_request(text, r);
                } catch (const std::exception& e) {
                    r.error = std::string("bad request: ") + e.what();
                }
                std::lock_guard lock(m_mutex);
                m_inbound.push_back(std::move(r));
            }
            ++k;
        }
        flush_outbound();
    }
    flush_outbound();
    for (auto& c : clients) {
        shutdown(c->socket, SD_SEND);
        closesocket(c->socket);
    }
}

#else
#error "ControlServer is only implemented for Windows so far"
#endif

} // namespace spud::engine
