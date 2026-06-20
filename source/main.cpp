/*
 * PS5 Direct Package Installer (singleDPI)
 *
 * Derived from the etaHEN Direct Package Installer implementation.
 * Copyright (C) 2025 etaHEN / LightningMods
 * Standalone refactor and modifications Copyright (C) 2026 MaxMilu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "appinst.hpp"
#include "tiny-json.hpp"

#include <arpa/inet.h>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kPort = 9090;
constexpr std::size_t kRequestSize = 16 * 1024;
constexpr std::size_t kJsonTokens = 128;
constexpr int kProtocolVersion = 2;
constexpr const char* kServiceName = "singleDPI";
constexpr int kDebugAuthIdProbeProtection = PROT_READ | PROT_WRITE | PROT_EXEC;
constexpr unsigned long kDebugAuthId = 0x4800000000000006UL;

enum class ApiError : int {
    KstuffUnavailable = -1,
    AuthIdUnavailable = -2,
    AppInstUnavailable = -3,
    MissingUrl = -4,
    MissingContentId = -5,
    InvalidJson = -10,
    UnknownAction = -11,
};

struct NotificationRequest {
    char reserved[45];
    char message[3075];
};

extern "C" int sceKernelMprotect(void* address, std::size_t size, int protection);
extern "C" int sceKernelSendNotificationRequest(
    int device,
    NotificationRequest* request,
    std::size_t size,
    int blocking);
extern "C" unsigned long kernel_get_ucred_authid(int pid);
extern "C" int kernel_set_ucred_authid(int pid, unsigned long authid);

struct RuntimeState {
    bool kstuff_available = false;
    bool authid_available = false;
    bool appinst_available = false;
    unsigned long original_authid = 0;
    unsigned long current_authid = 0;
    char last_content_id[kContentIdSize]{};
};

RuntimeState g_runtime;

void notify(const char* format, ...) {
    NotificationRequest request{};

    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(request.message, sizeof(request.message), format, arguments);
    va_end(arguments);

    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    std::printf("[singleDPI] %s\n", request.message);
}

std::string escape_json(const char* input) {
    std::string result;
    if (!input) {
        return result;
    }

    for (const unsigned char ch : std::string(input)) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                result += escaped;
            } else {
                result += static_cast<char>(ch);
            }
        }
    }
    return result;
}

const char* get_string(const json_t* json, const char* name, const char* fallback = "") {
    const char* value = json_getPropertyValue(json, name);
    return value ? value : fallback;
}

std::string error_response(ApiError code, const char* message) {
    return std::string("{\"res\":") + std::to_string(static_cast<int>(code)) +
        ",\"error\":\"" + escape_json(message) + "\"}";
}

bool probe_required_rwx_capability() {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 0x4000;
    }

    void* page = mmap(nullptr, static_cast<std::size_t>(page_size),
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) {
        return false;
    }

    const int result = sceKernelMprotect(
        page, static_cast<std::size_t>(page_size), kDebugAuthIdProbeProtection);
    if (result == 0) {
        sceKernelMprotect(page, static_cast<std::size_t>(page_size), PROT_READ | PROT_WRITE);
    }
    munmap(page, static_cast<std::size_t>(page_size));
    return result == 0;
}

bool prepare_appinst_authid(RuntimeState& runtime) {
    runtime.original_authid = kernel_get_ucred_authid(getpid());
    if (runtime.original_authid == 0) {
        return false;
    }

    if (runtime.original_authid != kDebugAuthId &&
        kernel_set_ucred_authid(getpid(), kDebugAuthId) != 0) {
        return false;
    }

    runtime.current_authid = kernel_get_ucred_authid(getpid());
    return runtime.current_authid == kDebugAuthId;
}

PlayGoInfo make_playgo_info() {
    PlayGoInfo info{};
    return info;
}

void initialize_runtime(RuntimeState& runtime) {
    runtime.kstuff_available = probe_required_rwx_capability();
    if (runtime.kstuff_available) {
        runtime.authid_available = prepare_appinst_authid(runtime);
    }
    if (runtime.authid_available) {
        runtime.appinst_available = sceAppInstUtilInitialize() == 0;
    }
}

bool is_ready(const RuntimeState& runtime) {
    return runtime.kstuff_available && runtime.authid_available &&
        runtime.appinst_available;
}

const char* runtime_message(const RuntimeState& runtime) {
    if (!runtime.kstuff_available) {
        return "kstuff capability is unavailable";
    }
    if (!runtime.authid_available) {
        return "AppInst debug AuthID is unavailable";
    }
    if (!runtime.appinst_available) {
        return "AppInst initialization failed";
    }
    return "ready to install packages";
}

std::string capabilities_response(const RuntimeState& runtime) {
    char authids[192];
    std::snprintf(authids, sizeof(authids),
        "\"authid_available\":%s,\"original_authid\":\"0x%016lx\","
        "\"current_authid\":\"0x%016lx\",",
        runtime.authid_available ? "true" : "false",
        runtime.original_authid,
        runtime.current_authid);

    return std::string("{\"res\":0,\"service\":\"") + kServiceName +
        "\",\"version\":" + std::to_string(kProtocolVersion) + "," +
        "\"ready\":" + (is_ready(runtime) ? "true" : "false") + "," +
        "\"message\":\"" + escape_json(runtime_message(runtime)) + "\"," +
        "\"kstuff_available\":" + (runtime.kstuff_available ? "true" : "false") + "," +
        authids +
        "\"appinst_available\":" + (runtime.appinst_available ? "true" : "false") + "}";
}

std::string install_package(const json_t* json, RuntimeState& runtime) {
    if (!runtime.kstuff_available) {
        return error_response(ApiError::KstuffUnavailable,
            "kstuff capability not detected; load compatible kstuff first");
    }
    if (!runtime.authid_available) {
        return error_response(ApiError::AuthIdUnavailable,
            "failed to acquire the required AppInst debug AuthID");
    }
    if (!runtime.appinst_available) {
        return error_response(ApiError::AppInstUnavailable,
            "sceAppInstUtilInitialize failed; payload lacks AppInst permissions");
    }

    const char* url = get_string(json, "url");
    if (!url[0]) {
        return error_response(ApiError::MissingUrl, "url is required");
    }

    const char* title = get_string(json, "content_name", nullptr);
    if (!title) {
        title = get_string(json, "title", "singleDPI");
    }

    MetaInfo metadata{
        .uri = url,
        .ex_uri = get_string(json, "ex_uri"),
        .playgo_scenario_id = get_string(json, "playgo_scenario_id"),
        .content_id = get_string(json, "content_id"),
        .content_name = title,
        .icon_url = get_string(json, "icon_url"),
    };

    SceAppInstallPkgInfo package_info{};
    PlayGoInfo playgo_info = make_playgo_info();
    const int result = sceAppInstUtilInstallByPackage(
        &metadata, &package_info, &playgo_info);

    if (result != 0) {
        notify("singleDPI - Install failed\nTitle: %.160s\nError: 0x%08X",
            title, static_cast<unsigned int>(result));

        char response[256];
        std::snprintf(response, sizeof(response),
            "{\"res\":%d,\"error\":\"sceAppInstUtilInstallByPackage failed\"}", result);
        return response;
    }

    std::snprintf(runtime.last_content_id, sizeof(runtime.last_content_id), "%s",
        package_info.content_id);
    notify("singleDPI - Installation started\nTitle: %.160s\nContent ID: %.47s",
        title, runtime.last_content_id);

    return std::string("{\"res\":0,\"content_id\":\"") +
        escape_json(runtime.last_content_id) + "\",\"title\":\"" +
        escape_json(title) +
        "\",\"message\":\"installation task started\"}";
}

std::string package_status(const json_t* json, const RuntimeState& runtime) {
    const char* content_id = get_string(json, "content_id", runtime.last_content_id);
    if (!content_id[0]) {
        return error_response(ApiError::MissingContentId, "content_id is required");
    }

    SceAppInstallStatusInstalled status{};
    const int result = sceAppInstUtilGetInstallStatus(content_id, &status);
    if (result != 0) {
        char response[256];
        std::snprintf(response, sizeof(response),
            "{\"res\":%d,\"error\":\"sceAppInstUtilGetInstallStatus failed\"}", result);
        return response;
    }

    const double progress = status.total_size == 0
        ? 0.0
        : (static_cast<double>(status.downloaded_size) * 100.0 /
            static_cast<double>(status.total_size));

    char numbers[768];
    std::snprintf(numbers, sizeof(numbers),
        "\"remain_time\":%u,\"downloaded_size\":%llu,\"total_size\":%llu,"
        "\"progress\":%.2f,\"promote_progress\":%u,\"local_copy_percent\":%d,"
        "\"is_copy_only\":%s,\"error_code\":%d",
        status.remain_time,
        static_cast<unsigned long long>(status.downloaded_size),
        static_cast<unsigned long long>(status.total_size),
        progress,
        status.promote_progress,
        status.local_copy_percent,
        status.is_copy_only ? "true" : "false",
        status.error_info.error_code);

    return std::string("{\"res\":0,\"content_id\":\"") +
        escape_json(content_id) + "\",\"status\":\"" +
        escape_json(status.status) + "\",\"source_type\":\"" +
        escape_json(status.src_type) + "\",\"error_type\":\"" +
        escape_json(status.error_info.type) + "\",\"error_description\":\"" +
        escape_json(status.error_info.description) + "\"," + numbers + "}";
}

std::string handle_request(char* request, RuntimeState& runtime) {
    json_t tokens[kJsonTokens]{};
    const json_t* json = json_create(request, tokens, kJsonTokens);
    if (!json) {
        return error_response(ApiError::InvalidJson, "invalid JSON");
    }

    const char* action = get_string(json, "action");
    if (!action[0]) {
        action = "install"; // Backward compatibility with the original DPI API.
    }

    if (std::strcmp(action, "ping") == 0) {
        return capabilities_response(runtime);
    }
    if (std::strcmp(action, "status") == 0) {
        return package_status(json, runtime);
    }
    if (std::strcmp(action, "install") == 0) {
        return install_package(json, runtime);
    }
    return error_response(ApiError::UnknownAction, "unknown action");
}

bool send_all(int socket_fd, const std::string& response) {
    std::size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t result = send(socket_fd, response.data() + sent,
            response.size() - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

int create_server() {
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        return -1;
    }

    int enabled = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    setsockopt(server, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kPort);

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server, 4) < 0) {
        close(server);
        return -1;
    }
    return server;
}

} // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);

    initialize_runtime(g_runtime);

    const int server = create_server();
    if (server < 0) {
        notify("singleDPI - Startup failed\nCannot listen on TCP port %d", kPort);
        return 1;
    }

    if (is_ready(g_runtime)) {
        notify("singleDPI v%d - Ready\nDirect Package Installer\nTCP port: %d",
            kProtocolVersion, kPort);
    } else {
        notify("singleDPI v%d - Not ready\n%s\nTCP port: %d",
            kProtocolVersion, runtime_message(g_runtime), kPort);
    }

    for (;;) {
        sockaddr_in client_address{};
        socklen_t client_size = sizeof(client_address);
        const int client = accept(server,
            reinterpret_cast<sockaddr*>(&client_address), &client_size);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        char request[kRequestSize]{};
        const ssize_t received = recv(client, request, sizeof(request) - 1, 0);
        if (received > 0) {
            request[received] = '\0';
            const std::string response = handle_request(request, g_runtime);
            send_all(client, response);
        }
        close(client);
    }

    close(server);
    return 0;
}
