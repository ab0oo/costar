#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// Lightweight JSON parsing primitives — no external dependencies, no widget state.
namespace dsl_json {

size_t skipWs(std::string_view text, size_t i);
std::string_view trimView(std::string_view text);

bool parseQuotedString(std::string_view text, size_t quotePos, std::string& out, size_t& nextPos);
bool findValueEnd(std::string_view text, size_t start, size_t& endPos);

bool objectMemberValue(std::string_view objectText, std::string_view key, std::string_view& out);
bool objectMemberString(std::string_view objectText, const char* key, std::string& out);
bool objectMemberInt(std::string_view objectText, const char* key, int& out);
bool objectMemberBool(std::string_view objectText, const char* key, bool& out);
bool objectMemberObject(std::string_view objectText, const char* key, std::string_view& out);
bool objectMemberArray(std::string_view objectText, const char* key, std::string_view& out);

bool arrayElementValue(std::string_view arrayText, int index, std::string_view& out);

bool parseStrictDouble(const std::string& text, double& out);
bool viewToString(std::string_view valueText, std::string& out);
bool viewToInt(std::string_view valueText, int& out);
bool viewToBool(std::string_view valueText, bool& out);
bool viewToDouble(std::string_view valueText, double& out);

// Template helpers for iterating objects/arrays — defined here to allow
// inlining of the callback without a virtual dispatch.
template <typename Fn>
void forEachObjectMember(std::string_view objectText, Fn&& fn) {
  objectText = trimView(objectText);
  if (objectText.size() < 2 || objectText.front() != '{' || objectText.back() != '}') {
    return;
  }

  size_t i = 1;
  while (i + 1 < objectText.size()) {
    i = skipWs(objectText, i);
    if (i >= objectText.size() - 1 || objectText[i] == '}') {
      break;
    }
    if (objectText[i] != '"') {
      return;
    }

    std::string memberKey;
    size_t keyEnd = 0;
    if (!parseQuotedString(objectText, i, memberKey, keyEnd)) {
      return;
    }

    i = skipWs(objectText, keyEnd);
    if (i >= objectText.size() || objectText[i] != ':') {
      return;
    }

    ++i;
    const size_t valueStart = skipWs(objectText, i);
    size_t valueEnd = 0;
    if (!findValueEnd(objectText, valueStart, valueEnd)) {
      return;
    }

    const std::string_view valueText = objectText.substr(valueStart, valueEnd - valueStart);
    fn(memberKey, valueText);

    i = skipWs(objectText, valueEnd);
    if (i < objectText.size() && objectText[i] == ',') {
      ++i;
    }
  }
}

template <typename Fn>
void forEachArrayElement(std::string_view arrayText, Fn&& fn) {
  arrayText = trimView(arrayText);
  if (arrayText.size() < 2 || arrayText.front() != '[' || arrayText.back() != ']') {
    return;
  }

  int idx = 0;
  size_t i = 1;
  while (i + 1 < arrayText.size()) {
    i = skipWs(arrayText, i);
    if (i >= arrayText.size() - 1 || arrayText[i] == ']') {
      break;
    }

    size_t valueEnd = 0;
    if (!findValueEnd(arrayText, i, valueEnd)) {
      return;
    }

    fn(idx, arrayText.substr(i, valueEnd - i));
    ++idx;

    i = skipWs(arrayText, valueEnd);
    if (i < arrayText.size() && arrayText[i] == ',') {
      ++i;
    }
  }
}

uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b);
bool parseHexColor565(const std::string& hex, uint16_t& outColor);

std::string replaceAll(std::string input, const std::string& needle, const std::string& value);

}  // namespace dsl_json
