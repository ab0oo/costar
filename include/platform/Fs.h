#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#include <FS.h>
#endif

namespace platform::fs {

bool begin(bool formatOnFail = true);
bool exists(const char* path);
bool mkdir(const char* path);
bool remove(const char* path);
bool rename(const char* from, const char* to);

#ifdef ARDUINO
using File = ::fs::File;
bool exists(const String& path);
bool mkdir(const String& path);
bool remove(const String& path);
bool rename(const String& from, const String& to);
File open(const String& path, const char* mode = FILE_READ);
#endif

}  // namespace platform::fs
