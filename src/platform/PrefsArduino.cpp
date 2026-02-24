#include "platform/Prefs.h"

#include <Preferences.h>
#include <cstring>

namespace platform::prefs {

bool getBool(const char* ns, const char* key, bool defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }
  Preferences prefs;
  if (!prefs.begin(ns, false)) {
    return defaultValue;
  }
  const bool value = prefs.getBool(key, defaultValue);
  prefs.end();
  return value;
}

uint32_t getUInt(const char* ns, const char* key, uint32_t defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }
  Preferences prefs;
  if (!prefs.begin(ns, false)) {
    return defaultValue;
  }
  const uint32_t value = prefs.getUInt(key, defaultValue);
  prefs.end();
  return value;
}

int32_t getInt(const char* ns, const char* key, int32_t defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }
  Preferences prefs;
  if (!prefs.begin(ns, true)) {
    return defaultValue;
  }
  const int32_t value = prefs.getInt(key, defaultValue);
  prefs.end();
  return value;
}

float getFloat(const char* ns, const char* key, float defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }
  Preferences prefs;
  if (!prefs.begin(ns, true)) {
    return defaultValue;
  }
  const float value = prefs.getFloat(key, defaultValue);
  prefs.end();
  return value;
}

std::string getString(const char* ns, const char* key, const char* defaultValue) {
  if (defaultValue == nullptr) {
    defaultValue = "";
  }
  if (ns == nullptr || key == nullptr) {
    return std::string(defaultValue);
  }
  Preferences prefs;
  if (!prefs.begin(ns, true)) {
    return std::string(defaultValue);
  }
  const String value = prefs.getString(key, defaultValue);
  prefs.end();
  return std::string(value.c_str());
}

bool putBool(const char* ns, const char* key, bool value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(ns, false)) {
    return false;
  }
  const bool ok = prefs.putBool(key, value) > 0;
  prefs.end();
  return ok;
}

bool putUInt(const char* ns, const char* key, uint32_t value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(ns, false)) {
    return false;
  }
  const bool ok = prefs.putUInt(key, value) > 0;
  prefs.end();
  return ok;
}

bool putInt(const char* ns, const char* key, int32_t value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(ns, false)) {
    return false;
  }
  const bool ok = prefs.putInt(key, value) > 0;
  prefs.end();
  return ok;
}

bool putFloat(const char* ns, const char* key, float value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(ns, false)) {
    return false;
  }
  const bool ok = prefs.putFloat(key, value) > 0;
  prefs.end();
  return ok;
}

bool putString(const char* ns, const char* key, const char* value) {
  if (ns == nullptr || key == nullptr || value == nullptr) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(ns, false)) {
    return false;
  }
  const bool ok = prefs.putString(key, value) > 0;
  prefs.end();
  return ok;
}

}  // namespace platform::prefs
