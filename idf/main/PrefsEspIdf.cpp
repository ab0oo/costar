#include "platform/Prefs.h"

#include <cstring>

#include "nvs.h"

namespace platform::prefs {

bool getBool(const char* ns, const char* key, bool defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }

  nvs_handle_t handle = 0;
  const esp_err_t openErr = nvs_open(ns, NVS_READONLY, &handle);
  if (openErr != ESP_OK) {
    return defaultValue;
  }

  uint8_t raw = defaultValue ? 1U : 0U;
  const esp_err_t getErr = nvs_get_u8(handle, key, &raw);
  nvs_close(handle);
  if (getErr != ESP_OK && getErr != ESP_ERR_NVS_NOT_FOUND) {
    return defaultValue;
  }
  return raw != 0U;
}

uint32_t getUInt(const char* ns, const char* key, uint32_t defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }

  nvs_handle_t handle = 0;
  const esp_err_t openErr = nvs_open(ns, NVS_READONLY, &handle);
  if (openErr != ESP_OK) {
    return defaultValue;
  }

  uint32_t value = defaultValue;
  const esp_err_t getErr = nvs_get_u32(handle, key, &value);
  nvs_close(handle);
  if (getErr != ESP_OK && getErr != ESP_ERR_NVS_NOT_FOUND) {
    return defaultValue;
  }
  return value;
}

int32_t getInt(const char* ns, const char* key, int32_t defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }

  nvs_handle_t handle = 0;
  const esp_err_t openErr = nvs_open(ns, NVS_READONLY, &handle);
  if (openErr != ESP_OK) {
    return defaultValue;
  }

  int32_t value = defaultValue;
  const esp_err_t getErr = nvs_get_i32(handle, key, &value);
  nvs_close(handle);
  if (getErr != ESP_OK && getErr != ESP_ERR_NVS_NOT_FOUND) {
    return defaultValue;
  }
  return value;
}

float getFloat(const char* ns, const char* key, float defaultValue) {
  if (ns == nullptr || key == nullptr) {
    return defaultValue;
  }

  nvs_handle_t handle = 0;
  const esp_err_t openErr = nvs_open(ns, NVS_READONLY, &handle);
  if (openErr != ESP_OK) {
    return defaultValue;
  }

  uint32_t raw = 0;
  std::memcpy(&raw, &defaultValue, sizeof(raw));
  const esp_err_t getErr = nvs_get_u32(handle, key, &raw);
  nvs_close(handle);
  if (getErr != ESP_OK && getErr != ESP_ERR_NVS_NOT_FOUND) {
    return defaultValue;
  }
  float value = defaultValue;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

std::string getString(const char* ns, const char* key, const char* defaultValue) {
  if (defaultValue == nullptr) {
    defaultValue = "";
  }
  if (ns == nullptr || key == nullptr) {
    return std::string(defaultValue);
  }

  nvs_handle_t handle = 0;
  const esp_err_t openErr = nvs_open(ns, NVS_READONLY, &handle);
  if (openErr != ESP_OK) {
    return std::string(defaultValue);
  }

  size_t size = 0;
  const esp_err_t lenErr = nvs_get_str(handle, key, nullptr, &size);
  if (lenErr == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return std::string(defaultValue);
  }
  if (lenErr != ESP_OK || size == 0) {
    nvs_close(handle);
    return std::string(defaultValue);
  }

  std::string value(size, '\0');
  const esp_err_t getErr = nvs_get_str(handle, key, value.data(), &size);
  nvs_close(handle);
  if (getErr != ESP_OK || value.empty()) {
    return std::string(defaultValue);
  }
  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value;
}

bool putBool(const char* ns, const char* key, bool value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }

  nvs_handle_t handle = 0;
  if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  const esp_err_t setErr = nvs_set_u8(handle, key, value ? 1U : 0U);
  if (setErr != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  const esp_err_t commitErr = nvs_commit(handle);
  nvs_close(handle);
  return commitErr == ESP_OK;
}

bool putUInt(const char* ns, const char* key, uint32_t value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }

  nvs_handle_t handle = 0;
  if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  const esp_err_t setErr = nvs_set_u32(handle, key, value);
  if (setErr != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  const esp_err_t commitErr = nvs_commit(handle);
  nvs_close(handle);
  return commitErr == ESP_OK;
}

bool putInt(const char* ns, const char* key, int32_t value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }

  nvs_handle_t handle = 0;
  if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  const esp_err_t setErr = nvs_set_i32(handle, key, value);
  if (setErr != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  const esp_err_t commitErr = nvs_commit(handle);
  nvs_close(handle);
  return commitErr == ESP_OK;
}

bool putFloat(const char* ns, const char* key, float value) {
  if (ns == nullptr || key == nullptr) {
    return false;
  }

  uint32_t raw = 0;
  std::memcpy(&raw, &value, sizeof(raw));
  return putUInt(ns, key, raw);
}

bool putString(const char* ns, const char* key, const char* value) {
  if (ns == nullptr || key == nullptr || value == nullptr) {
    return false;
  }

  nvs_handle_t handle = 0;
  if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  const esp_err_t setErr = nvs_set_str(handle, key, value);
  if (setErr != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  const esp_err_t commitErr = nvs_commit(handle);
  nvs_close(handle);
  return commitErr == ESP_OK;
}

}  // namespace platform::prefs
