#include "peer.hpp"
#include "protocol.hpp"
#include "../core/registry.hpp"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#define close(s) closesocket(s)
#ifdef _MSC_VER
typedef int ssize_t;
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#endif
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <random>
#include <sstream>
#include <thread>
#include <iostream>

namespace axon::mcp {

using json = nlohmann::json;

static std::atomic<bool> g_peer_running{false};
static std::thread g_peer_thread;
static int g_peer_fd = -1;

static int peer_timeout_ms() {
    constexpr int fallback = 15000;
    const char* raw = std::getenv("AXON_PEER_TIMEOUT_MS");
    if (!raw || !*raw) return fallback;
    try {
        int value = std::stoi(raw);
        return std::max(100, std::min(value, 300000));
    } catch (...) {
        return fallback;
    }
}

static void set_socket_timeout(int fd, int timeout_ms) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#else
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#endif
}

static long long self_pid() {
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}

bool pid_alive(long long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

// send() can short-write on large payloads, and writing to a peer-closed
// socket raises SIGPIPE — fatal for a stdio serve that never installed a
// handler. Loop until done and suppress the signal per-call.
static bool send_all(int fd, const char* data, size_t len) {
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
#if defined(SO_NOSIGPIPE)
    int no_sigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&no_sigpipe),
               sizeof(no_sigpipe));
#endif
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, flags);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static std::string generate_token() {
    std::random_device rd;
    std::ostringstream oss;
    oss << std::hex;
    for (int i = 0; i < 4; i++)
        oss << rd();
    return oss.str();
}

// Reads an HTTP/1.1 request until headers + Content-Length bytes of body are
// in, so proxied tool arguments larger than one recv() survive intact.
static bool read_http_request(int fd, std::string& request, size_t max_bytes = 4 * 1024 * 1024) {
    char buf[8192];
    size_t header_end = std::string::npos;
    size_t content_length = 0;

    while (request.size() < max_bytes) {
        if (header_end == std::string::npos) {
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string lower;
                lower.reserve(header_end);
                for (size_t i = 0; i < header_end; i++)
                    lower.push_back(
                        static_cast<char>(std::tolower(static_cast<unsigned char>(request[i]))));
                auto pos = lower.find("content-length:");
                if (pos != std::string::npos) {
                    try {
                        content_length = std::stoul(lower.substr(pos + 15));
                    } catch (...) {
                        content_length = 0;
                    }
                }
            }
        }
        if (header_end != std::string::npos && request.size() >= header_end + 4 + content_length)
            return true;

        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return header_end != std::string::npos;
        request.append(buf, static_cast<size_t>(n));
    }
    return false;
}

static void send_http_response(int fd, int status, const std::string& body) {
    const char* status_text = status == 200   ? "OK"
                              : status == 401 ? "Unauthorized"
                              : status == 404 ? "Not Found"
                                              : "Bad Request";
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << status_text << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    std::string response = oss.str();
    send_all(fd, response.c_str(), response.size());
}

static std::string header_value(const std::string& request, const std::string& name) {
    std::istringstream ss(request);
    std::string line;
    std::getline(ss, line);
    std::string lower_name = name;
    for (auto& c : lower_name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (auto& c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key != lower_name) continue;
        size_t start = colon + 1;
        while (start < line.size() && line[start] == ' ')
            start++;
        return line.substr(start);
    }
    return "";
}

static void handle_peer_client(int client_fd, ServerContext& ctx) {
    std::string request;
    if (!read_http_request(client_fd, request)) {
        close(client_fd);
        return;
    }

    std::istringstream rl(request);
    std::string method, path, version;
    rl >> method >> path >> version;

    if (method == "GET" && path == "/rpc/health") {
        std::lock_guard<std::mutex> lock(*ctx.tool_mutex);
        auto idle_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - ctx.last_tool_activity)
                                .count();
        send_http_response(client_fd, 200,
                           json{{"pid", self_pid()},
                                {"root", ctx.cfg.project_root.string()},
                                {"db_ready", ctx.db_ready()},
                                {"idle_seconds", std::max<int64_t>(0, idle_seconds)}}
                               .dump());
        close(client_fd);
        return;
    }

    if (method == "POST" && path == "/rpc/tool") {
        std::string auth = header_value(request, "authorization");
        if (auth != "Bearer " + ctx.peer_token) {
            send_http_response(client_fd, 401, json{{"error", "invalid token"}}.dump());
            close(client_fd);
            return;
        }

        auto body_start = request.find("\r\n\r\n");
        std::string body = body_start == std::string::npos ? "" : request.substr(body_start + 4);
        json payload;
        try {
            payload = json::parse(body);
        } catch (...) {
            send_http_response(client_fd, 400, json{{"error", "invalid JSON body"}}.dump());
            close(client_fd);
            return;
        }

        std::string name = payload.value("name", "");
        json args = payload.value("arguments", json::object());
        json result;
        try {
            result = call_tool(name, args, ctx);
        } catch (const std::exception& e) {
            result = make_tool_result({{"error", std::string(e.what())}}, true);
        }
        send_http_response(client_fd, 200, result.dump());
        close(client_fd);
        return;
    }

    send_http_response(client_fd, 404, json{{"error", "not found"}}.dump());
    close(client_fd);
}

bool start_peer_listener(ServerContext& ctx) {
    if (g_peer_running.load()) return true;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return false;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (getsockname(fd, (sockaddr*)&bound, &len) < 0) {
        close(fd);
        return false;
    }

    ctx.peer_port = ntohs(bound.sin_port);
    ctx.peer_token = generate_token();
    g_peer_fd = fd;
    g_peer_running = true;

    g_peer_thread = std::thread([fd, &ctx]() {
        while (g_peer_running.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv{1, 0};
            if (select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            int client_fd = accept(fd, nullptr, nullptr);
            if (client_fd < 0) continue;
            handle_peer_client(client_fd, ctx);
        }
    });
    return true;
}

void stop_peer_listener() {
    if (!g_peer_running.exchange(false)) return;
    if (g_peer_thread.joinable()) g_peer_thread.join();
    if (g_peer_fd >= 0) {
        close(g_peer_fd);
        g_peer_fd = -1;
    }
}

// Minimal localhost HTTP request. Reads until the server closes the connection
// (the peer listener and web server both send Connection: close).
static std::optional<std::string> http_request_local(int port, const std::string& method,
                                                     const std::string& path,
                                                     const std::string& token,
                                                     const std::string& body, std::string& error,
                                                     int timeout_ms = 0) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket() failed";
        return std::nullopt;
    }

    if (timeout_ms <= 0) timeout_ms = peer_timeout_ms();
    set_socket_timeout(fd, timeout_ms);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        error = "connect to 127.0.0.1:" + std::to_string(port) + " failed";
        close(fd);
        return std::nullopt;
    }

    std::ostringstream oss;
    oss << method << " " << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1:" << port << "\r\n"
        << "Connection: close\r\n";
    if (!token.empty()) oss << "Authorization: Bearer " << token << "\r\n";
    if (method == "POST")
        oss << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    oss << "\r\n" << body;
    std::string request = oss.str();
    if (!send_all(fd, request.c_str(), request.size())) {
        error = "send failed";
        close(fd);
        return std::nullopt;
    }

    std::string response;
    char buf[8192];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
        response.append(buf, static_cast<size_t>(n));
    close(fd);

    if (response.empty()) {
        error = "owner did not respond within " + std::to_string(timeout_ms) + "ms";
        return std::nullopt;
    }

    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        error = response.empty() ? "empty response" : "malformed response";
        return std::nullopt;
    }

    std::istringstream sl(response);
    std::string http_version;
    int status = 0;
    sl >> http_version >> status;
    if (status != 200) {
        error = "owner returned HTTP " + std::to_string(status);
        return std::nullopt;
    }
    return response.substr(body_start + 4);
}

std::optional<json> proxy_tool_call(ServerContext& ctx, const std::string& name, const json& args,
                                    std::string& error) {
    auto entry = find_repo(ctx.cfg.project_root.string());
    if (!entry || entry->owner_pid == 0 || entry->owner_port == 0) {
        error = "no live owner registered for this repo";
        return std::nullopt;
    }
    if (entry->owner_pid == self_pid()) {
        error = "registry owner is this process";
        return std::nullopt;
    }
    if (!pid_alive(entry->owner_pid)) {
        error =
            "registered owner (pid " + std::to_string(entry->owner_pid) + ") is no longer running";
        return std::nullopt;
    }

    json payload = {{"name", name}, {"arguments", args}};
    auto body = http_request_local(entry->owner_port, "POST", "/rpc/tool", entry->owner_token,
                                   payload.dump(), error);
    if (!body) return std::nullopt;

    try {
        json result = json::parse(*body);
        if (result.contains("content")) return result;
        error = "owner response missing tool result envelope";
        return std::nullopt;
    } catch (...) {
        error = "owner response is not valid JSON";
        return std::nullopt;
    }
}

std::optional<json> probe_peer_health(int port, std::string& error) {
    auto body = http_request_local(port, "GET", "/rpc/health", "", "", error, 1000);
    if (!body) return std::nullopt;
    try {
        return json::parse(*body);
    } catch (...) {
        error = "owner health response is not valid JSON";
        return std::nullopt;
    }
}

void register_self_as_owner(ServerContext& ctx, int port) {
    if (ctx.owner_registered || port == 0) return;
    if (ctx.peer_token.empty()) ctx.peer_token = generate_token();
    set_repo_owner(ctx.cfg.project_root.string(), self_pid(), port, ctx.peer_token);
    ctx.owner_registered = true;
}

void unregister_self_as_owner(ServerContext& ctx) {
    if (!ctx.owner_registered) return;
    clear_repo_owner(ctx.cfg.project_root.string(), self_pid());
    ctx.owner_registered = false;
}

void heartbeat_self_as_owner(ServerContext& ctx) {
    if (!ctx.owner_registered) return;
    if (!touch_repo_owner(ctx.cfg.project_root.string(), self_pid())) ctx.owner_registered = false;
}

} // namespace axon::mcp
