#pragma once

#include <cstdint>
#include <string>

namespace platform::prefs {

bool getBool(const char* ns, const char* key, bool defaultValue);
uint32_t getUInt(const char* ns, const char* key, uint32_t defaultValue);
int32_t getInt(const char* ns, const char* key, int32_t defaultValue);
float getFloat(const char* ns, const char* key, float defaultValue);
std::string getString(const char* ns, const char* key, const char* defaultValue = "");

bool putBool(const char* ns, const char* key, bool value);
bool putUInt(const char* ns, const char* key, uint32_t value);
bool putInt(const char* ns, const char* key, int32_t value);
bool putFloat(const char* ns, const char* key, float value);
bool putString(const char* ns, const char* key, const char* value);

}  // namespace platform::prefs
