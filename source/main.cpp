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
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "cache.hpp"

namespace {

constexpr int kPort = 9090;
constexpr int kDpiV2Port = 12800;
constexpr std::size_t kRequestSize = 16 * 1024;
constexpr std::size_t kDpiV2RequestSize = 64 * 1024;
constexpr std::size_t kJsonTokens = 128;
constexpr const char* kVersion = "0.1.0";
constexpr const char* kServiceName = "singleDPI";
constexpr const char* kProcessName = "singleDPI.elf";
constexpr std::int32_t kSystemServiceParamLanguage = 1;
constexpr std::int32_t kLanguageChineseTraditional = 10;
constexpr std::int32_t kLanguageChineseSimplified = 11;
constexpr int kDebugAuthIdProbeProtection = PROT_READ | PROT_WRITE | PROT_EXEC;
constexpr unsigned long kDebugAuthId = 0x4800000000000006UL;

enum class ApiError : int {
    KstuffUnavailable = -1,
    AuthIdUnavailable = -2,
    AppInstUnavailable = -3,
    MissingUrl = -4,
    MissingContentId = -5,
    MissingTitleId = -6,
    MissingCategory = -7,
    InvalidInstallQuery = -8,
    UnsupportedCategory = -9,
    InvalidJson = -10,
    UnknownAction = -11,
};

enum class NotificationLanguage {
    English,
    ChineseSimplified,
    ChineseTraditional,
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
extern "C" int sceSystemServiceParamGetInt(
    std::int32_t parameter_id,
    std::int32_t* value);
extern "C" std::uint32_t kernel_get_fw_version();
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
NotificationLanguage g_notification_language = NotificationLanguage::English;

void notify(const char* format, ...) {
    NotificationRequest request{};

    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(request.message, sizeof(request.message), format, arguments);
    va_end(arguments);

    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    std::printf("[singleDPI] %s\n", request.message);
}

NotificationLanguage detect_notification_language() {
    std::int32_t language = 0;
    if (sceSystemServiceParamGetInt(kSystemServiceParamLanguage, &language) != 0) {
        return NotificationLanguage::English;
    }

    if (language == kLanguageChineseSimplified) {
        return NotificationLanguage::ChineseSimplified;
    }
    if (language == kLanguageChineseTraditional) {
        return NotificationLanguage::ChineseTraditional;
    }
    return NotificationLanguage::English;
}

const char* notification_language_code(NotificationLanguage language) {
    switch (language) {
    case NotificationLanguage::ChineseSimplified: return "zh-Hans";
    case NotificationLanguage::ChineseTraditional: return "zh-Hant";
    default: return "en";
    }
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

std::string firmware_version_string() {
    const std::uint32_t fw = kernel_get_fw_version();
    const std::uint32_t major_bcd = (fw >> 24) & 0xFFu;
    const std::uint32_t minor_bcd = (fw >> 16) & 0xFFu;
    const std::uint32_t major =
        ((major_bcd >> 4) & 0xFu) * 10u + (major_bcd & 0xFu);
    const std::uint32_t minor =
        ((minor_bcd >> 4) & 0xFu) * 10u + (minor_bcd & 0xFu);

    if (major == 0 && minor == 0) {
        return "unknown";
    }

    char out[16];
    std::snprintf(out, sizeof(out), "%u.%02u", major, minor);
    return out;
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

const char* localized_runtime_message(
    const RuntimeState& runtime,
    NotificationLanguage language) {
    if (language == NotificationLanguage::ChineseSimplified) {
        if (!runtime.kstuff_available) return "未检测到 kstuff 所需能力";
        if (!runtime.authid_available) return "无法获取 AppInst 调试 AuthID";
        if (!runtime.appinst_available) return "AppInst 初始化失败";
        return "可以安装 PKG";
    }
    if (language == NotificationLanguage::ChineseTraditional) {
        if (!runtime.kstuff_available) return "未偵測到 kstuff 所需能力";
        if (!runtime.authid_available) return "無法取得 AppInst 偵錯 AuthID";
        if (!runtime.appinst_available) return "AppInst 初始化失敗";
        return "可以安裝 PKG";
    }
    return runtime_message(runtime);
}

void notify_install_failed(const char* title, int error) {
    if (g_notification_language == NotificationLanguage::ChineseSimplified) {
        notify("singleDPI - 安装失败\n标题：%s\n错误：0x%08X",
            title, static_cast<unsigned int>(error));
    } else if (g_notification_language == NotificationLanguage::ChineseTraditional) {
        notify("singleDPI - 安裝失敗\n標題：%s\n錯誤：0x%08X",
            title, static_cast<unsigned int>(error));
    } else {
        notify("singleDPI - Install failed\nTitle: %s\nError: 0x%08X",
            title, static_cast<unsigned int>(error));
    }
}

void notify_install_started(const char* title, const char* content_id) {
    if (g_notification_language == NotificationLanguage::ChineseSimplified) {
        notify("singleDPI - 安装任务已开始\n标题：%s\n内容 ID：%.47s",
            title, content_id);
    } else if (g_notification_language == NotificationLanguage::ChineseTraditional) {
        notify("singleDPI - 安裝工作已開始\n標題：%s\n內容 ID：%.47s",
            title, content_id);
    } else {
        notify("singleDPI - Installation started\nTitle: %s\nContent ID: %.47s",
            title, content_id);
    }
}

std::string capabilities_response(const RuntimeState& runtime) {
    char authids[192];
    std::snprintf(authids, sizeof(authids),
        "\"authid_available\":%s,\"original_authid\":\"0x%016lx\","
        "\"current_authid\":\"0x%016lx\",",
        runtime.authid_available ? "true" : "false",
        runtime.original_authid,
        runtime.current_authid);

    const CacheInfo ci = cache_info();

    return std::string("{\"res\":0,\"service\":\"") + kServiceName +
        "\",\"version\":\"" + kVersion + "\"," +
        "\"firmware_version\":\"" + escape_json(firmware_version_string().c_str()) + "\"," +
        "\"dpi_v1_port\":" + std::to_string(kPort) + "," +
        "\"dpi_v2_url_port\":" + std::to_string(kDpiV2Port) + "," +
        "\"dpi_v2_url_available\":true," +
        "\"notification_language\":\"" +
            notification_language_code(g_notification_language) + "\"," +
        "\"ready\":" + (is_ready(runtime) ? "true" : "false") + "," +
        "\"message\":\"" + escape_json(runtime_message(runtime)) + "\"," +
        "\"kstuff_available\":" + (runtime.kstuff_available ? "true" : "false") + "," +
        authids +
        "\"appinst_available\":" + (runtime.appinst_available ? "true" : "false") + "," +
        "\"icon_cache_size_bytes\":" + std::to_string(ci.total_size_bytes) + "}";
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

    const char* content_id = get_string(json, "content_id", "");
    const char* icon_url  = get_string(json, "icon_url", "");

    // Attempt icon caching — on failure keep the original remote URL.
    std::string local_icon_url;
    if (content_id[0] && icon_url[0]) {
        local_icon_url = cache_download_icon(content_id, icon_url);
        if (!local_icon_url.empty())
            icon_url = local_icon_url.c_str();
        else
            std::printf("[singleDPI] icon cache miss, using remote URL\n");
    }

    MetaInfo metadata{
        .uri = url,
        .ex_uri = get_string(json, "ex_uri"),
        .playgo_scenario_id = get_string(json, "playgo_scenario_id"),
        .content_id = content_id,
        .content_name = title,
        .icon_url = icon_url,
    };

    SceAppInstallPkgInfo package_info{};
    PlayGoInfo playgo_info = make_playgo_info();
    const int result = sceAppInstUtilInstallByPackage(
        &metadata, &package_info, &playgo_info);

    if (result != 0) {
        notify_install_failed(title, result);

        char response[256];
        std::snprintf(response, sizeof(response),
            "{\"res\":%d,\"error\":\"sceAppInstUtilInstallByPackage failed\"}", result);
        return response;
    }

    std::snprintf(runtime.last_content_id, sizeof(runtime.last_content_id), "%s",
        package_info.content_id);
    notify_install_started(title, runtime.last_content_id);

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

bool is_safe_path_component(const char* value) {
    if (!value || !value[0]) {
        return false;
    }

    for (const unsigned char ch : std::string(value)) {
        if ((ch < 'A' || ch > 'Z') && (ch < 'a' || ch > 'z') &&
            (ch < '0' || ch > '9') && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

std::string installed_package_response(const json_t* json) {
    const char* title_id = get_string(json, "title_id");
    const char* category = get_string(json, "category");
    const char* content_id = get_string(json, "content_id");

    if (!title_id[0]) {
        return error_response(ApiError::MissingTitleId, "title_id is required");
    }
    if (!category[0]) {
        return error_response(ApiError::MissingCategory, "category is required");
    }
    if (!is_safe_path_component(title_id) || std::strlen(title_id) > 32) {
        return error_response(ApiError::InvalidInstallQuery, "invalid title_id");
    }

    std::string category_root;
    std::string title_root;
    std::string parent_path;
    std::string path;
    if (std::strcmp(category, "gd") == 0 ||
        std::strcmp(category, "gda") == 0 ||
        std::strcmp(category, "la") == 0) {
        category_root = "/user/app";
        title_root = category_root + "/" + title_id;
        parent_path = title_root;
        path = parent_path + "/app.json";
    } else if (std::strcmp(category, "gp") == 0) {
        category_root = "/user/patch";
        title_root = category_root + "/" + title_id;
        parent_path = title_root;
        path = parent_path + "/patch.json";
    } else if (std::strcmp(category, "ac") == 0) {
        const std::size_t content_id_length = std::strlen(content_id);
        if (content_id_length < 16 || content_id_length > 64 ||
            !is_safe_path_component(content_id)) {
            return error_response(ApiError::InvalidInstallQuery,
                "DLC requires a valid content_id");
        }
        const char* content_label = content_id + content_id_length - 16;
        category_root = "/user/addcont";
        title_root = category_root + "/" + title_id;
        parent_path = title_root + "/" + content_label;
        path = parent_path + "/ac.json";
    } else {
        return error_response(ApiError::UnsupportedCategory,
            "unsupported package category");
    }

    struct stat info{};
    const bool user_root_visible = stat("/user", &info) == 0;
    const bool category_root_visible = stat(category_root.c_str(), &info) == 0;
    const bool title_root_visible = stat(title_root.c_str(), &info) == 0;
    const bool parent_visible = stat(parent_path.c_str(), &info) == 0;

    errno = 0;
    const bool exists = stat(path.c_str(), &info) == 0;
    const int stat_error = exists ? 0 : errno;
    const char* stat_error_description = exists ? "" : std::strerror(stat_error);

    return std::string("{\"res\":0,\"exists\":") +
        (exists ? "true" : "false") +
        ",\"title_id\":\"" + escape_json(title_id) +
        "\",\"content_id\":\"" + escape_json(content_id) +
        "\",\"category\":\"" + escape_json(category) +
        "\",\"path\":\"" + escape_json(path.c_str()) +
        "\",\"user_root_visible\":" + (user_root_visible ? "true" : "false") +
        ",\"category_root_visible\":" + (category_root_visible ? "true" : "false") +
        ",\"title_root_visible\":" + (title_root_visible ? "true" : "false") +
        ",\"parent_visible\":" + (parent_visible ? "true" : "false") +
        ",\"errno\":" + std::to_string(stat_error) +
        ",\"error_description\":\"" +
            escape_json(stat_error_description) + "\"}";
}

std::string cache_info_response() {
    const CacheInfo ci = cache_info();

    std::string json = "{\"res\":0,\"cache_dir\":\"" + escape_json(ci.dir.c_str()) +
        "\",\"file_count\":" + std::to_string(ci.files.size()) +
        ",\"total_size_bytes\":" + std::to_string(ci.total_size_bytes) +
        ",\"files\":[";

    for (std::size_t i = 0; i < ci.files.size(); ++i) {
        if (i > 0) json += ",";
        json += "{\"name\":\"" + escape_json(ci.files[i].name.c_str()) + "\"" +
            ",\"size\":" + std::to_string(ci.files[i].size_bytes) + "}";
    }

    json += "]}";
    return json;
}

std::string cache_clear_response() {
    std::uint64_t freed = 0;
    const bool ok = cache_clear(&freed);

    return std::string("{\"res\":") + (ok ? "0" : "-1") +
        ",\"message\":\"" + escape_json(ok ? "cache cleared" : "cache clear failed") + "\"" +
        ",\"bytes_freed\":" + std::to_string(freed) + "}";
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
    if (std::strcmp(action, "is_installed") == 0) {
        return installed_package_response(json);
    }
    if (std::strcmp(action, "install") == 0) {
        return install_package(json, runtime);
    }
    if (std::strcmp(action, "cache_info") == 0) {
        return cache_info_response();
    }
    if (std::strcmp(action, "cache_clear") == 0) {
        return cache_clear_response();
    }
    return error_response(ApiError::UnknownAction, "unknown action");
}

std::string url_decode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out += ' ';
        } else if (value[i] == '%' && i + 2 < value.size() &&
            std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            char hex[3] = { value[i + 1], value[i + 2], '\0' };
            out += static_cast<char>(std::strtoul(hex, nullptr, 16));
            i += 2;
        } else {
            out += value[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parse_urlencoded_form(const std::string& body) {
    std::map<std::string, std::string> fields;
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t end = body.find('&', start);
        const std::string pair = body.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        const std::size_t equals = pair.find('=');
        if (equals != std::string::npos) {
            fields[url_decode(pair.substr(0, equals))] =
                url_decode(pair.substr(equals + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

std::map<std::string, std::string> parse_multipart_text_form(
    const std::string& body,
    const std::string& content_type) {
    std::map<std::string, std::string> fields;
    const std::string marker = "boundary=";
    const std::size_t boundary_pos = content_type.find(marker);
    if (boundary_pos == std::string::npos) {
        return fields;
    }

    std::string boundary = content_type.substr(boundary_pos + marker.size());
    if (!boundary.empty() && boundary.front() == '"') {
        boundary.erase(0, 1);
        const std::size_t quote = boundary.find('"');
        if (quote != std::string::npos) {
            boundary.erase(quote);
        }
    } else {
        const std::size_t semicolon = boundary.find(';');
        if (semicolon != std::string::npos) {
            boundary.erase(semicolon);
        }
    }
    boundary = "--" + boundary;

    std::size_t part_start = body.find(boundary);
    while (part_start != std::string::npos) {
        part_start += boundary.size();
        if (part_start + 2 <= body.size() && body.compare(part_start, 2, "--") == 0) {
            break;
        }
        if (part_start + 2 <= body.size() && body.compare(part_start, 2, "\r\n") == 0) {
            part_start += 2;
        }

        const std::size_t headers_end = body.find("\r\n\r\n", part_start);
        if (headers_end == std::string::npos) {
            break;
        }

        const std::string headers = body.substr(part_start, headers_end - part_start);
        const std::size_t name_pos = headers.find("name=\"");
        if (name_pos != std::string::npos) {
            const std::size_t name_start = name_pos + 6;
            const std::size_t name_end = headers.find('"', name_start);
            const std::size_t value_start = headers_end + 4;
            const std::size_t next_boundary = body.find(boundary, value_start);
            if (name_end != std::string::npos && next_boundary != std::string::npos) {
                std::string value = body.substr(value_start, next_boundary - value_start);
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
                    value.pop_back();
                }
                fields[headers.substr(name_start, name_end - name_start)] = value;
            }
        }

        part_start = body.find(boundary, headers_end + 4);
    }
    return fields;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string http_header_value(const std::string& request, const std::string& header) {
    const std::string lower_request = lowercase_ascii(request);
    const std::string lower_header = lowercase_ascii(header) + ":";
    const std::size_t pos = lower_request.find(lower_header);
    if (pos == std::string::npos) {
        return "";
    }
    std::size_t value_start = pos + lower_header.size();
    while (value_start < request.size() &&
        (request[value_start] == ' ' || request[value_start] == '\t')) {
        ++value_start;
    }
    const std::size_t value_end = request.find("\r\n", value_start);
    return request.substr(value_start,
        value_end == std::string::npos ? std::string::npos : value_end - value_start);
}

std::string http_response(const std::string& body, const char* status = "200 OK") {
    return std::string("HTTP/1.1 ") + status + "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string single_dpi_install_json_from_fields(const std::map<std::string, std::string>& fields) {
    auto get = [&](const char* key) -> std::string {
        const auto it = fields.find(key);
        return it == fields.end() ? "" : it->second;
    };
    const std::string content_name = get("content_name").empty()
        ? "singleDPI DPIv2"
        : get("content_name");

    return std::string("{\"action\":\"install\",\"url\":\"") +
        escape_json(get("url").c_str()) +
        "\",\"content_name\":\"" + escape_json(content_name.c_str()) +
        "\",\"content_id\":\"" + escape_json(get("content_id").c_str()) +
        "\",\"playgo_scenario_id\":\"" + escape_json(get("playgo_scenario_id").c_str()) +
        "\",\"ex_uri\":\"" + escape_json(get("ex_uri").c_str()) +
        "\",\"icon_url\":\"" + escape_json(get("icon_url").c_str()) + "\"}";
}

std::string handle_dpi_v2_http_request(const std::string& request, RuntimeState& runtime) {
    if (request.rfind("OPTIONS ", 0) == 0) {
        return http_response("OK");
    }
    if (request.rfind("GET ", 0) == 0) {
        return http_response("singleDPI DPI v2 URL endpoint is ready. POST a form field named url.");
    }
    if (request.rfind("POST ", 0) != 0) {
        return http_response("FAILED: Method not allowed", "405 Method Not Allowed");
    }

    const std::size_t body_start = request.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        return http_response("FAILED: Invalid HTTP request", "400 Bad Request");
    }

    const std::string content_type = http_header_value(request, "Content-Type");
    const std::string body = request.substr(body_start + 4);
    std::map<std::string, std::string> fields;
    if (lowercase_ascii(content_type).find("multipart/form-data") != std::string::npos) {
        fields = parse_multipart_text_form(body, content_type);
    } else {
        fields = parse_urlencoded_form(body);
    }

    if (fields.find("url") == fields.end() || fields["url"].empty()) {
        return http_response("FAILED: url field is required", "400 Bad Request");
    }

    std::string install_json = single_dpi_install_json_from_fields(fields);
    const std::string appinst_response = handle_request(install_json.data(), runtime);

    json_t response_tokens[kJsonTokens]{};
    std::string mutable_response = appinst_response;
    const json_t* response_json = json_create(mutable_response.data(), response_tokens, kJsonTokens);
    const char* res_text = response_json ? json_getPropertyValue(response_json, "res") : nullptr;
    const char* content_id = response_json ? json_getPropertyValue(response_json, "content_id") : nullptr;
    const int res = res_text ? std::atoi(res_text) : -1;

    if (res == 0) {
        return http_response("SUCCESS: Direct install console Task started for URL: " +
            fields["url"] + "\n" + "{\"res\":0,\"content_id\":\"" +
            escape_json(content_id ? content_id : "") +
            "\",\"dpi_mode\":\"v2_url\"}");
    }
    return http_response("FAILED: Install failed with response " + appinst_response);
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

int create_server(int port = kPort) {
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
    address.sin_port = htons(port);

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server, 4) < 0) {
        close(server);
        return -1;
    }
    return server;
}

void handle_json_client(int client) {
    char request[kRequestSize]{};
    const ssize_t received = recv(client, request, sizeof(request) - 1, 0);
    if (received > 0) {
        request[received] = '\0';
        const std::string response = handle_request(request, g_runtime);
        send_all(client, response);
    }
}

void handle_dpi_v2_client(int client) {
    std::string request;
    request.reserve(kDpiV2RequestSize);
    char buffer[4096];
    for (;;) {
        const ssize_t received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        request.append(buffer, static_cast<std::size_t>(received));
        const std::size_t header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const std::string content_length_text =
                http_header_value(request, "Content-Length");
            const std::size_t content_length = content_length_text.empty()
                ? 0
                : static_cast<std::size_t>(std::strtoull(content_length_text.c_str(), nullptr, 10));
            if (request.size() >= header_end + 4 + content_length) {
                break;
            }
        }
        if (request.size() >= kDpiV2RequestSize) {
            break;
        }
    }

    const std::string response = handle_dpi_v2_http_request(request, g_runtime);
    send_all(client, response);
}

} // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);

    syscall(SYS_thr_set_name, -1, kProcessName);

    cache_init_dirs();
    if (!cache_icon_server_start()) {
        std::printf("[singleDPI] warning: icon cache server failed to start\n");
    }

    g_notification_language = detect_notification_language();
    initialize_runtime(g_runtime);

    // Keep the diagnostic API available even when package installation is not ready.
    if (!is_ready(g_runtime)) {
        if (g_notification_language == NotificationLanguage::ChineseSimplified) {
            notify("singleDPI %s - 尚未就绪\n原因：%s\n载荷进程将退出",
                kVersion, localized_runtime_message(g_runtime, g_notification_language));
        } else if (g_notification_language == NotificationLanguage::ChineseTraditional) {
            notify("singleDPI %s - 尚未就緒\n原因：%s\n載荷行程將退出",
                kVersion, localized_runtime_message(g_runtime, g_notification_language));
        } else {
            notify("singleDPI %s - Not ready\n%s\nDiagnostic API remains available",
                kVersion, runtime_message(g_runtime));
        }
    }

    const int server = create_server();
    if (server < 0) {
        if (g_notification_language == NotificationLanguage::ChineseSimplified) {
            notify("singleDPI - 启动失败\n无法监听 TCP 端口 %d", kPort);
        } else if (g_notification_language == NotificationLanguage::ChineseTraditional) {
            notify("singleDPI - 啟動失敗\n無法監聽 TCP 連接埠 %d", kPort);
        } else {
            notify("singleDPI - Startup failed\nCannot listen on TCP port %d", kPort);
        }
        cache_icon_server_stop();
        return 1;
    }

    const int dpi_v2_server = create_server(kDpiV2Port);
    if (dpi_v2_server < 0) {
        std::printf("[singleDPI] warning: DPI v2 URL server failed to listen on TCP port %d\n",
            kDpiV2Port);
    } else {
        std::printf("[singleDPI] DPI v2 URL endpoint listening on TCP port %d\n", kDpiV2Port);
    }

    if (is_ready(g_runtime) && g_notification_language == NotificationLanguage::ChineseSimplified) {
        notify("singleDPI %s - 已就绪\n远程 PKG 安装服务\nTCP 端口：%d",
            kVersion, kPort);
    } else if (is_ready(g_runtime) && g_notification_language == NotificationLanguage::ChineseTraditional) {
        notify("singleDPI %s - 已就緒\n遠端 PKG 安裝服務\nTCP 連接埠：%d",
            kVersion, kPort);
    } else if (is_ready(g_runtime)) {
        notify("singleDPI %s - Ready\nDirect Package Installer\nTCP port: %d",
            kVersion, kPort);
    }

    for (;;) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server, &read_fds);
        int max_fd = server;
        if (dpi_v2_server >= 0) {
            FD_SET(dpi_v2_server, &read_fds);
            if (dpi_v2_server > max_fd) {
                max_fd = dpi_v2_server;
            }
        }

        const int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (FD_ISSET(server, &read_fds)) {
            sockaddr_in client_address{};
            socklen_t client_size = sizeof(client_address);
            const int client = accept(server,
                reinterpret_cast<sockaddr*>(&client_address), &client_size);
            if (client >= 0) {
                handle_json_client(client);
                close(client);
            }
        }

        if (dpi_v2_server >= 0 && FD_ISSET(dpi_v2_server, &read_fds)) {
            sockaddr_in client_address{};
            socklen_t client_size = sizeof(client_address);
            const int client = accept(dpi_v2_server,
                reinterpret_cast<sockaddr*>(&client_address), &client_size);
            if (client >= 0) {
                handle_dpi_v2_client(client);
                close(client);
            }
        }
    }

    if (dpi_v2_server >= 0) {
        close(dpi_v2_server);
    }
    close(server);
    cache_icon_server_stop();
    return 0;
}
