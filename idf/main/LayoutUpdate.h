#pragma once

#include <cstdint>

// Poll-on-boot layout update check.
//
// Fetches GET /manifest/<mac> from the layout server, compares per-file MD5s
// against values stored in NVS, downloads any changed files to LittleFS, and
// returns true if any file was updated (caller should call esp_restart()).
//
// All transfers are plain HTTP — no TLS required. The layout server must be
// reachable before calling this (i.e. WiFi already connected).

namespace layout_update {

struct Config {
  const char* host;     // e.g. "vps.gorkos.net"
  uint16_t    port;     // e.g. 8087
  uint32_t    timeoutMs = 10000;
};

// Returns true if one or more files were downloaded and written to LittleFS.
// Returns false if the server is unreachable, has no layout for this device,
// all files are up to date, or any error occurs (errors are logged, not fatal).
bool checkAndApply(const Config& cfg);

} // namespace layout_update
