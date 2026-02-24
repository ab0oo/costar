#include "platform/Fs.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_littlefs.h"

namespace {
constexpr const char* kBasePath = "/littlefs";
constexpr const char* kPartitionLabel = "storage";
bool sMounted = false;

bool buildPath(const char* input, char* out, size_t outLen) {
  if (input == nullptr || out == nullptr || outLen == 0) {
    return false;
  }
  if (input[0] == '\0') {
    return false;
  }
  if (std::strncmp(input, kBasePath, std::strlen(kBasePath)) == 0) {
    return std::snprintf(out, outLen, "%s", input) > 0;
  }
  if (input[0] == '/') {
    return std::snprintf(out, outLen, "%s%s", kBasePath, input) > 0;
  }
  return std::snprintf(out, outLen, "%s/%s", kBasePath, input) > 0;
}
}  // namespace

namespace platform::fs {

bool begin(bool formatOnFail) {
  if (sMounted) {
    return true;
  }

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = kBasePath;
  conf.partition_label = kPartitionLabel;
  conf.format_if_mount_failed = formatOnFail;
  conf.dont_mount = false;

  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    return false;
  }

  sMounted = true;
  return true;
}

bool exists(const char* path) {
  char full[256];
  if (!buildPath(path, full, sizeof(full))) {
    return false;
  }
  struct stat st = {};
  return stat(full, &st) == 0;
}

bool mkdir(const char* path) {
  char full[256];
  if (!buildPath(path, full, sizeof(full))) {
    return false;
  }
  return ::mkdir(full, 0755) == 0 || errno == EEXIST;
}

bool remove(const char* path) {
  char full[256];
  if (!buildPath(path, full, sizeof(full))) {
    return false;
  }
  return ::unlink(full) == 0;
}

bool rename(const char* from, const char* to) {
  char src[256];
  char dst[256];
  if (!buildPath(from, src, sizeof(src)) || !buildPath(to, dst, sizeof(dst))) {
    return false;
  }
  return ::rename(src, dst) == 0;
}

}  // namespace platform::fs
