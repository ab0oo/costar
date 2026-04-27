#include "LayoutUpdate.h"

#include "DslJson.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mbedtls/md5.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace layout_update {

namespace {

constexpr const char* kTag        = "layout-update";
constexpr const char* kNvsNs      = "layout_md5";  // NVS namespace for stored hashes
constexpr size_t      kMaxFileBytes = 32768;        // max single file download (32 KB)
constexpr size_t      kMaxManifest  = 4096;         // manifest JSON is tiny

// ── HTTP helpers ─────────────────────────────────────────────────────────────

static bool httpGet(const char* url, uint32_t timeoutMs, std::string& outBody, int& outStatus) {
    outBody.clear();
    outStatus = 0;

    esp_http_client_config_t cfg = {};
    cfg.url        = url;
    cfg.method     = HTTP_METHOD_GET;
    cfg.timeout_ms = static_cast<int>(timeoutMs);
    cfg.buffer_size = 1024;
    cfg.user_agent  = "CoStar-LayoutUpdate/1.0";
    // Plain HTTP only — layout server is on the local network, no TLS needed.

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(kTag, "http_client_init failed url=%s", url);
        return false;
    }

    bool ok = false;
    const esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        const int64_t contentLen = esp_http_client_fetch_headers(client);
        outStatus = esp_http_client_get_status_code(client);
        if (outStatus == 200) {
            const size_t cap = (contentLen > 0 && static_cast<size_t>(contentLen) <= kMaxFileBytes)
                               ? static_cast<size_t>(contentLen) : 1024U;
            outBody.reserve(cap);
            char buf[512];
            for (;;) {
                const int n = esp_http_client_read(client, buf, sizeof(buf));
                if (n > 0) {
                    if (outBody.size() + static_cast<size_t>(n) > kMaxFileBytes) {
                        ESP_LOGW(kTag, "response too large url=%s", url);
                        break;
                    }
                    outBody.append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n == 0) { ok = true; }
                break;
            }
        }
    } else {
        ESP_LOGW(kTag, "http open failed err=%s url=%s", esp_err_to_name(err), url);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok && outStatus == 200;
}

// ── MD5 helpers ───────────────────────────────────────────────────────────────

// Returns lowercase hex MD5 of data, or "" on failure.
static std::string md5Hex(const void* data, size_t len) {
    unsigned char digest[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts(&ctx) != 0 ||
        mbedtls_md5_update(&ctx, static_cast<const unsigned char*>(data), len) != 0 ||
        mbedtls_md5_finish(&ctx, digest) != 0) {
        mbedtls_md5_free(&ctx);
        return "";
    }
    mbedtls_md5_free(&ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", static_cast<unsigned>(digest[i]));
    }
    return std::string(hex, 32);
}

// ── NVS MD5 store ─────────────────────────────────────────────────────────────

// NVS key is derived from filename (max 15 chars for NVS key limit).
static std::string nvsKey(const std::string& filename) {
    // screen_layout_1.json → sl1, tabs.json → tabs, wifi.json → wifi, etc.
    std::string k;
    for (char c : filename) {
        if (c == '.') break;          // strip extension
        if (c == '_') continue;       // strip underscores
        if (k.size() >= 14) break;
        k += c;
    }
    return k.empty() ? filename.substr(0, 14) : k;
}

static std::string nvsGetMd5(const std::string& filename) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return "";
    char buf[33] = {};
    size_t len = sizeof(buf);
    nvs_get_str(h, nvsKey(filename).c_str(), buf, &len);
    nvs_close(h);
    return std::string(buf);
}

static void nvsPutMd5(const std::string& filename, const std::string& md5) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, nvsKey(filename).c_str(), md5.c_str());
    nvs_commit(h);
    nvs_close(h);
}

// ── MAC address ───────────────────────────────────────────────────────────────

static std::string wifiMac() {
    uint8_t mac[6] = {};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) return "";
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

// ── LittleFS write ────────────────────────────────────────────────────────────

static bool writeFile(const char* path, const std::string& data) {
    std::FILE* fp = std::fopen(path, "wb");
    if (fp == nullptr) {
        ESP_LOGE(kTag, "fopen failed path=%s", path);
        return false;
    }
    const size_t written = std::fwrite(data.data(), 1, data.size(), fp);
    std::fclose(fp);
    if (written != data.size()) {
        ESP_LOGE(kTag, "write short path=%s wrote=%u of=%u",
                 path, static_cast<unsigned>(written), static_cast<unsigned>(data.size()));
        return false;
    }
    return true;
}

// ── Manifest parsing ──────────────────────────────────────────────────────────

struct FileEntry {
    std::string name;
    std::string md5;
};

// Parse: {"layout_id":N,"files":{"name":{"md5":"...","size":N},...}}
static std::vector<FileEntry> parseManifest(const std::string& json) {
    std::vector<FileEntry> entries;
    using namespace dsl_json;

    std::string_view filesObj;
    if (!objectMemberObject(json, "files", filesObj)) {
        ESP_LOGW(kTag, "manifest: no 'files' object");
        return entries;
    }

    // Walk each key in the files object
    forEachObjectMember(std::string(filesObj), [&](std::string_view key, std::string_view valSv) {
        std::string valStr(valSv);
        std::string md5;
        if (objectMemberString(valStr, "md5", md5) && !md5.empty()) {
            entries.push_back({ std::string(key), std::move(md5) });
        }
    });

    return entries;
}

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────────

bool checkAndApply(const Config& cfg) {
    if (cfg.host == nullptr || cfg.host[0] == '\0') {
        ESP_LOGW(kTag, "no layout server host configured; skipping update check");
        return false;
    }

    const std::string mac = wifiMac();
    if (mac.empty()) {
        ESP_LOGW(kTag, "could not read WiFi MAC; skipping update check");
        return false;
    }

    // Build manifest URL: http://host:port[/path]/manifest/<MAC>
    const char* pfx = (cfg.path && cfg.path[0]) ? cfg.path : "";
    char manifestUrl[160];
    std::snprintf(manifestUrl, sizeof(manifestUrl),
                  "http://%s:%u%s/manifest/%s", cfg.host, cfg.port, pfx, mac.c_str());

    ESP_LOGI(kTag, "checking manifest url=%s", manifestUrl);
    std::string manifestBody;
    int status = 0;
    if (!httpGet(manifestUrl, cfg.timeoutMs, manifestBody, status)) {
        if (status == 404) {
            ESP_LOGI(kTag, "no layout assigned to this device (404); skipping");
        } else {
            ESP_LOGW(kTag, "manifest fetch failed status=%d", status);
        }
        return false;
    }

    const std::vector<FileEntry> entries = parseManifest(manifestBody);
    if (entries.empty()) {
        ESP_LOGW(kTag, "manifest empty or unparseable");
        return false;
    }
    ESP_LOGI(kTag, "manifest has %u file(s)", static_cast<unsigned>(entries.size()));

    bool anyUpdated = false;

    for (const FileEntry& entry : entries) {
        const std::string storedMd5 = nvsGetMd5(entry.name);
        if (storedMd5 == entry.md5) {
            ESP_LOGI(kTag, "up-to-date file=%s md5=%s", entry.name.c_str(), entry.md5.c_str());
            continue;
        }

        // Fetch file: http://host:port[/path]/files/<MAC>/<filename>
        char fileUrl[192];
        std::snprintf(fileUrl, sizeof(fileUrl),
                      "http://%s:%u%s/files/%s/%s",
                      cfg.host, cfg.port, pfx, mac.c_str(), entry.name.c_str());

        ESP_LOGI(kTag, "downloading file=%s url=%s", entry.name.c_str(), fileUrl);
        std::string body;
        int fileStatus = 0;
        if (!httpGet(fileUrl, cfg.timeoutMs, body, fileStatus)) {
            ESP_LOGW(kTag, "download failed file=%s status=%d", entry.name.c_str(), fileStatus);
            continue;
        }

        // Verify MD5 of downloaded content
        const std::string gotMd5 = md5Hex(body.data(), body.size());
        if (gotMd5 != entry.md5) {
            ESP_LOGW(kTag, "md5 mismatch file=%s expected=%s got=%s",
                     entry.name.c_str(), entry.md5.c_str(), gotMd5.c_str());
            continue;
        }

        // Write to LittleFS
        std::string path = "/littlefs/" + entry.name;
        if (!writeFile(path.c_str(), body)) {
            continue;  // error already logged
        }

        nvsPutMd5(entry.name, entry.md5);
        ESP_LOGI(kTag, "updated file=%s bytes=%u md5=%s",
                 entry.name.c_str(), static_cast<unsigned>(body.size()), entry.md5.c_str());
        anyUpdated = true;
    }

    if (anyUpdated) {
        ESP_LOGI(kTag, "layout updated — reboot required");
    } else {
        ESP_LOGI(kTag, "all files current — no update needed");
    }
    return anyUpdated;
}

} // namespace layout_update
