/*
 * Icon cache for singleDPI
 * Copyright (C) 2026 MaxMilu
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Downloads icon PNGs from client-provided URLs via raw sockets, stores
 * them under /data/singleDPI/icons/, and runs a minimal HTTP server on
 * 127.0.0.1:9091 so the PS5 download UI can display the icon even after
 * the PC client disconnects.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr const char* kCacheBaseDir  = "/data/singleDPI";
constexpr const char* kCacheIconDir  = "/data/singleDPI/icons";
constexpr std::size_t kCacheHashLen  = 8;
constexpr int kIconPort = 9091;

// --- URL parsing -----------------------------------------------------------

struct UrlParts {
    std::string host;
    int         port = 80;
    std::string path;
    bool        secure = false;
    bool        valid = false;
};

UrlParts cache_parse_url(const char* url);

// --- HTTP GET (raw socket, zero dependencies) ------------------------------

// Returns empty vector on any error — the caller falls back to the original URL.
std::vector<std::uint8_t> cache_http_get(const char* url, int timeout_ms = 5000);

// --- Cache helpers ---------------------------------------------------------

// Build the local file path for a given content_id + icon_url pair.
// Format: /data/singleDPI/icons/icon_{content_id}_{hash8}.png
// hash8  = top 8 hex digits of a djb2 hash of the icon_url, so the same
//          remote URL always maps to the same local filename.
std::string cache_file_path(const char* content_id, const char* icon_url);

// Ensure /data/singleDPI and /data/singleDPI/icons exist.
void cache_init_dirs();

// Download icon_url → save to local file, return the local http://127.0.0.1:9091/... URL.
// Returns empty string on failure (caller keeps the original icon_url).
std::string cache_download_icon(const char* content_id, const char* icon_url);

// --- Local HTTP icon server (runs in background thread) ---------------------

// Start the icon HTTP server on 127.0.0.1:kIconPort. Safe to call multiple
// times - only one instance runs. Returns true once the port is listening.
bool cache_icon_server_start();

// Stop the icon HTTP server (called on shutdown).
void cache_icon_server_stop();

// --- Cache info / management -----------------------------------------------

struct CacheFileEntry {
    std::string    name;
    std::uint64_t  size_bytes = 0;
};

struct CacheInfo {
    std::string              dir;
    std::uint64_t            total_size_bytes = 0;
    std::vector<CacheFileEntry> files;
};

CacheInfo cache_info();
bool      cache_clear(std::uint64_t* bytes_freed_out = nullptr);
