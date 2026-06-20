/*
 * Icon cache implementation - bounded HTTP GET + local file storage.
 * Copyright (C) 2026 MaxMilu
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cache.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxIconBytes = 8 * 1024 * 1024;
constexpr int kHttpBufSize = 2048;

std::atomic<int> g_icon_server_fd{-1};
std::atomic<bool> g_icon_server_running{false};
pthread_t g_icon_server_thread{};
bool g_icon_server_thread_created = false;

std::uint32_t djb2(const char* input) {
    std::uint32_t hash = 5381;
    if (!input) return hash;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p)
        hash = ((hash << 5) + hash) ^ static_cast<std::uint32_t>(*p);
    return hash;
}

std::string hex8(std::uint32_t value) {
    char buf[kCacheHashLen + 1];
    std::snprintf(buf, sizeof(buf), "%08x", value);
    return buf;
}

bool mkdir_p(const char* path) {
    struct stat st{};
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

std::uint64_t file_size(const char* path);

bool file_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool is_valid_png_file(const char* path) {
    const std::uint64_t size = file_size(path);
    if (size < 8 || size > kMaxIconBytes) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    unsigned char signature[8]{};
    const bool read = std::fread(signature, 1, sizeof(signature), file) == sizeof(signature);
    std::fclose(file);
    const unsigned char expected[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return read && std::memcmp(signature, expected, sizeof(expected)) == 0;
}

std::uint64_t file_size(const char* path) {
    struct stat st{};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
        return 0;
    return static_cast<std::uint64_t>(st.st_size);
}

bool send_all(int fd, const void* data, std::size_t size) {
    const char* bytes = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t result = send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool is_cache_file_name(const char* name) {
    if (!name) return false;
    const std::string value(name);
    if (value.size() <= 9 || value.compare(0, 5, "icon_") != 0 ||
        value.compare(value.size() - 4, 4, ".png") != 0 ||
        value.find("..") != std::string::npos)
        return false;
    for (const unsigned char ch : value) {
        if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
            return false;
    }
    return true;
}

std::string lower_ascii(std::string value) {
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

bool parse_decimal_size(const std::string& text, std::size_t* value_out) {
    if (!value_out || text.empty()) return false;
    std::size_t value = 0;
    for (const unsigned char ch : text) {
        if (!std::isdigit(ch)) return false;
        const unsigned digit = ch - '0';
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *value_out = value;
    return true;
}

std::int64_t monotonic_milliseconds() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool parse_response_headers(const std::vector<char>& raw, std::size_t header_end,
                            std::size_t* content_length_out) {
    const std::string headers(raw.data(), header_end);
    const std::size_t status_end = headers.find('\n');
    if (status_end == std::string::npos) return false;
    const std::string status = headers.substr(0, status_end);
    const std::size_t first_space = status.find(' ');
    if (first_space == std::string::npos || status.size() < first_space + 5 ||
        status.compare(first_space + 1, 3, "200") != 0 ||
        (status[first_space + 4] != ' ' && status[first_space + 4] != '\r'))
        return false;

    bool has_png_type = false;
    bool has_length = false;
    std::size_t content_length = 0;
    std::size_t line_start = status_end + 1;
    while (line_start < headers.size()) {
        std::size_t line_end = headers.find('\n', line_start);
        if (line_end == std::string::npos) line_end = headers.size();
        std::string line = headers.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string lower = lower_ascii(line);
        if (lower.rfind("content-type:", 0) == 0) {
            const std::string type = lower.substr(13);
            has_png_type = type.find("image/png") != std::string::npos;
        } else if (lower.rfind("content-length:", 0) == 0) {
            std::string length = lower.substr(15);
            const std::size_t begin = length.find_first_not_of(" \t");
            const std::size_t end = length.find_last_not_of(" \t");
            if (begin == std::string::npos ||
                !parse_decimal_size(length.substr(begin, end - begin + 1), &content_length))
                return false;
            has_length = true;
        } else if (lower.rfind("transfer-encoding:", 0) == 0 &&
                   lower.find("identity") == std::string::npos) {
            return false;
        }
        line_start = line_end + 1;
    }
    if (!has_png_type || (has_length && (content_length == 0 || content_length > kMaxIconBytes)))
        return false;
    if (content_length_out) *content_length_out = has_length ? content_length : 0;
    return true;
}

bool find_header_end(const std::vector<char>& raw, std::size_t* end_out) {
    for (std::size_t i = 3; i < raw.size(); ++i) {
        if (raw[i - 3] == '\r' && raw[i - 2] == '\n' && raw[i - 1] == '\r' && raw[i] == '\n') {
            *end_out = i + 1;
            return true;
        }
    }
    return false;
}

void* icon_server_loop(void*) {
    const int server = g_icon_server_fd.load();
    while (g_icon_server_running.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        const int client = accept(server, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }

        const timeval tv{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        char request[kHttpBufSize]{};
        const ssize_t n = recv(client, request, sizeof(request) - 1, 0);
        std::string name;
        if (n > 0) {
            const std::string first_line(request, static_cast<std::size_t>(n));
            if (first_line.rfind("GET /", 0) == 0) {
                const std::size_t end = first_line.find(' ', 5);
                if (end != std::string::npos) name = first_line.substr(5, end - 5);
            }
        }

        if (!is_cache_file_name(name.c_str())) {
            const char response[] = "HTTP/1.0 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
            send_all(client, response, sizeof(response) - 1);
        } else {
            const std::string full_path = std::string(kCacheIconDir) + "/" + name;
            FILE* file = std::fopen(full_path.c_str(), "rb");
            if (!file) {
                const char response[] = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                send_all(client, response, sizeof(response) - 1);
            } else {
                const std::uint64_t size = file_size(full_path.c_str());
                char header[512];
                const int header_len = std::snprintf(header, sizeof(header),
                    "HTTP/1.0 200 OK\r\nContent-Type: image/png\r\n"
                    "Content-Length: %llu\r\nCache-Control: public, max-age=86400\r\n\r\n",
                    static_cast<unsigned long long>(size));
                bool ok = header_len > 0 && static_cast<std::size_t>(header_len) < sizeof(header) &&
                    send_all(client, header, static_cast<std::size_t>(header_len));
                char buffer[8192];
                while (ok) {
                    const std::size_t bytes = std::fread(buffer, 1, sizeof(buffer), file);
                    if (bytes == 0) break;
                    ok = send_all(client, buffer, bytes);
                }
                std::fclose(file);
            }
        }
        close(client);
    }

    const int fd = g_icon_server_fd.exchange(-1);
    if (fd >= 0) close(fd);
    g_icon_server_running.store(false);
    return nullptr;
}

} // namespace

UrlParts cache_parse_url(const char* url) {
    UrlParts parts{};
    if (!url || !url[0]) return parts;
    const std::string input(url);
    std::size_t pos = 0;
    if (input.rfind("http://", 0) == 0) {
        pos = 7;
    } else if (input.rfind("https://", 0) == 0) {
        pos = 8;
        parts.secure = true;
        parts.port = 443;
    } else {
        return parts;
    }

    const std::size_t path_start = input.find('/', pos);
    const std::size_t authority_end = path_start == std::string::npos ? input.size() : path_start;
    const std::size_t colon = input.find(':', pos);
    const std::size_t host_end = colon != std::string::npos && colon < authority_end ? colon : authority_end;
    parts.host = input.substr(pos, host_end - pos);
    if (parts.host.empty() || parts.host.find_first_of(" \t\r\n@") != std::string::npos)
        return parts;

    if (host_end < authority_end) {
        const std::string port_text = input.substr(host_end + 1, authority_end - host_end - 1);
        std::size_t port = 0;
        if (!parse_decimal_size(port_text, &port) || port == 0 || port > 65535)
            return parts;
        parts.port = static_cast<int>(port);
    }
    parts.path = path_start == std::string::npos ? "/" : input.substr(path_start);
    if (parts.path.find_first_of("\r\n") != std::string::npos) return parts;
    parts.valid = true;
    return parts;
}

std::vector<std::uint8_t> cache_http_get(const char* url, int timeout_ms) {
    const UrlParts parts = cache_parse_url(url);
    if (!parts.valid || parts.secure || timeout_ms <= 0) return {};

    addrinfo hints{};
    hints.ai_flags = AI_NUMERICSERV;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%d", parts.port);
    if (getaddrinfo(parts.host.c_str(), port_str, &hints, &result) != 0 || !result) return {};

    int sock = -1;
    for (addrinfo* address = result; address; address = address->ai_next) {
        sock = socket(address->ai_family, address->ai_socktype, 0);
        if (sock < 0) continue;
        const timeval tv{static_cast<long>(timeout_ms / 1000),
                         static_cast<long>((timeout_ms % 1000) * 1000)};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, address->ai_addr, address->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(result);
    if (sock < 0) return {};

    const std::string request = "GET " + parts.path + " HTTP/1.0\r\nHost: " + parts.host +
        "\r\nAccept: image/png\r\nConnection: close\r\n\r\n";
    if (!send_all(sock, request.data(), request.size())) {
        close(sock);
        return {};
    }

    std::vector<char> raw;
    raw.reserve(256 * 1024);
    std::size_t header_end = 0;
    std::size_t content_length = 0;
    bool headers_validated = false;
    const std::int64_t started = monotonic_milliseconds();
    const std::int64_t deadline = started < 0 ? -1 : started + timeout_ms;
    char buffer[4096];
    for (;;) {
        if (deadline >= 0) {
            const std::int64_t remaining = deadline - monotonic_milliseconds();
            if (remaining <= 0) {
                close(sock);
                return {};
            }
            const timeval remaining_tv{static_cast<long>(remaining / 1000),
                                       static_cast<long>((remaining % 1000) * 1000)};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &remaining_tv, sizeof(remaining_tv));
        }
        const ssize_t count = recv(sock, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            close(sock);
            return {};
        }
        if (count == 0) break;
        raw.insert(raw.end(), buffer, buffer + count);
        if (!headers_validated) {
            if (raw.size() > kMaxHeaderBytes) {
                close(sock);
                return {};
            }
            if (find_header_end(raw, &header_end)) {
                if (!parse_response_headers(raw, header_end, &content_length)) {
                    close(sock);
                    return {};
                }
                headers_validated = true;
            }
        }
        if (headers_validated && raw.size() - header_end > kMaxIconBytes) {
            close(sock);
            return {};
        }
    }
    close(sock);
    if (!headers_validated) return {};
    const std::size_t body_size = raw.size() - header_end;
    if (body_size == 0 || body_size > kMaxIconBytes ||
        (content_length != 0 && body_size != content_length))
        return {};
    const unsigned char png_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (body_size < sizeof(png_signature) ||
        std::memcmp(raw.data() + header_end, png_signature, sizeof(png_signature)) != 0)
        return {};
    return std::vector<std::uint8_t>(raw.begin() + static_cast<std::ptrdiff_t>(header_end), raw.end());
}

std::string cache_file_path(const char* content_id, const char* icon_url) {
    if (!content_id || !content_id[0] || !icon_url || !icon_url[0]) return "";
    std::string safe_id;
    safe_id.reserve(64);
    for (const unsigned char ch : std::string(content_id)) {
        if (safe_id.size() == 64) break;
        safe_id += std::isalnum(ch) || ch == '-' || ch == '_' ? static_cast<char>(ch) : '_';
    }
    if (safe_id.empty()) return "";
    return std::string(kCacheIconDir) + "/icon_" + safe_id + "_" + hex8(djb2(icon_url)) + ".png";
}

void cache_init_dirs() {
    if (!mkdir_p(kCacheBaseDir)) return;
    mkdir_p(kCacheIconDir);
}

std::string cache_download_icon(const char* content_id, const char* icon_url) {
    if (!g_icon_server_running.load()) return "";
    const std::string local_path = cache_file_path(content_id, icon_url);
    if (local_path.empty()) return "";
    const std::string file_name = local_path.substr(local_path.find_last_of('/') + 1);
    const std::string local_url = "http://127.0.0.1:" + std::to_string(kIconPort) + "/" + file_name;
    if (file_exists(local_path.c_str())) {
        if (is_valid_png_file(local_path.c_str())) return local_url;
        unlink(local_path.c_str());
    }

    const std::vector<std::uint8_t> data = cache_http_get(icon_url);
    if (data.empty()) return "";
    const std::string tmp_path = local_path + ".tmp";
    FILE* file = std::fopen(tmp_path.c_str(), "wb");
    if (!file) return "";
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), file);
    const bool closed = std::fclose(file) == 0;
    if (written != data.size() || !closed || rename(tmp_path.c_str(), local_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        return "";
    }
    return local_url;
}

bool cache_icon_server_start() {
    if (g_icon_server_running.load()) return true;
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return false;
    int enabled = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(kIconPort);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(server, 8) < 0) {
        close(server);
        return false;
    }
    g_icon_server_fd.store(server);
    g_icon_server_running.store(true);
    if (pthread_create(&g_icon_server_thread, nullptr, icon_server_loop, nullptr) != 0) {
        g_icon_server_running.store(false);
        g_icon_server_fd.store(-1);
        close(server);
        return false;
    }
    g_icon_server_thread_created = true;
    return true;
}

void cache_icon_server_stop() {
    if (!g_icon_server_thread_created) return;
    g_icon_server_running.store(false);
    const int server = g_icon_server_fd.load();
    if (server >= 0) shutdown(server, SHUT_RDWR);
    pthread_join(g_icon_server_thread, nullptr);
    g_icon_server_thread_created = false;
}

CacheInfo cache_info() {
    CacheInfo info;
    info.dir = kCacheIconDir;
    DIR* dir = opendir(kCacheIconDir);
    if (!dir) return info;
    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!is_cache_file_name(entry->d_name)) continue;
        const std::string full_path = std::string(kCacheIconDir) + "/" + entry->d_name;
        const std::uint64_t size = file_size(full_path.c_str());
        if (size == 0) continue;
        info.total_size_bytes += size;
        info.files.push_back({entry->d_name, size});
    }
    closedir(dir);
    return info;
}

bool cache_clear(std::uint64_t* bytes_freed_out) {
    std::uint64_t freed = 0;
    DIR* dir = opendir(kCacheIconDir);
    if (!dir) return false;
    dirent* entry;
    bool ok = true;
    while ((entry = readdir(dir)) != nullptr) {
        if (!is_cache_file_name(entry->d_name)) continue;
        const std::string full_path = std::string(kCacheIconDir) + "/" + entry->d_name;
        const std::uint64_t size = file_size(full_path.c_str());
        if (unlink(full_path.c_str()) == 0) freed += size;
        else ok = false;
    }
    closedir(dir);
    if (bytes_freed_out) *bytes_freed_out = freed;
    return ok;
}
