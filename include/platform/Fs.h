#pragma once

namespace platform::fs {

bool begin(bool formatOnFail = true);
bool exists(const char* path);
bool mkdir(const char* path);
bool remove(const char* path);
bool rename(const char* from, const char* to);

}  // namespace platform::fs
