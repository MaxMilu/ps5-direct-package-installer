#pragma once

#include <cstddef>
#include <cstdint>

constexpr std::size_t kContentIdSize = 0x30;
constexpr std::size_t kLanguageSize = 8;
constexpr std::size_t kPlayGoScenarioIdSize = 3;
constexpr std::size_t kLanguageCount = 30;
constexpr std::size_t kIdCount = 64;

using ContentId = char[kContentIdSize];
using Language = char[kLanguageSize];
using PlayGoScenarioId = char[kPlayGoScenarioIdSize];

struct SceAppInstallPkgInfo {
    ContentId content_id;
    int content_type;
    int content_platform;
};

struct MetaInfo {
    const char* uri;
    const char* ex_uri;
    const char* playgo_scenario_id;
    const char* content_id;
    const char* content_name;
    const char* icon_url;
};

struct PlayGoInfo {
    Language languages[kLanguageCount];
    PlayGoScenarioId playgo_scenario_ids[kIdCount];
    ContentId content_ids[kIdCount];
    long unknown[810];
};

struct SceAppInstallErrorInfo {
    std::int32_t error_code;
    std::int32_t version;
    char description[512];
    char type[9];
};

struct SceAppInstallStatusInstalled {
    char status[16];
    char src_type[8];
    std::uint32_t remain_time;
    std::uint64_t downloaded_size;
    std::uint64_t initial_chunk_size;
    std::uint64_t total_size;
    std::uint32_t promote_progress;
    SceAppInstallErrorInfo error_info;
    std::int32_t local_copy_percent;
    bool is_copy_only;
};

extern "C" {
int sceAppInstUtilInitialize();
int sceAppInstUtilInstallByPackage(
    MetaInfo* metadata,
    SceAppInstallPkgInfo* package_info,
    PlayGoInfo* playgo_info);
int sceAppInstUtilGetInstallStatus(
    const char* content_id,
    SceAppInstallStatusInstalled* status);
int sceAppInstUtilGetContentIdFromPkg(
    const char* package_path,
    char* content_id,
    bool* is_app);
}
