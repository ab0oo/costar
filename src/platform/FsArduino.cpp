#include "platform/Fs.h"

#include <LittleFS.h>

namespace platform::fs {

bool begin(bool formatOnFail) { return LittleFS.begin(formatOnFail); }

bool exists(const char* path) { return path != nullptr && LittleFS.exists(path); }
bool exists(const String& path) { return LittleFS.exists(path); }

bool mkdir(const char* path) { return path != nullptr && LittleFS.mkdir(path); }
bool mkdir(const String& path) { return LittleFS.mkdir(path); }

bool remove(const char* path) { return path != nullptr && LittleFS.remove(path); }
bool remove(const String& path) { return LittleFS.remove(path); }

bool rename(const char* from, const char* to) {
  return from != nullptr && to != nullptr && LittleFS.rename(from, to);
}
bool rename(const String& from, const String& to) { return LittleFS.rename(from, to); }

File open(const String& path, const char* mode) { return LittleFS.open(path, mode); }

}  // namespace platform::fs
