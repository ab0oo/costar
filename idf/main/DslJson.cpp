// DslJson.cpp — lightweight JSON parsing primitives for DSL widget runtime.
// All functions are stateless and have no dependencies on widget state.

#include "DslJson.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace dsl_json {

size_t skipWs(std::string_view text, size_t i) {
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    ++i;
  }
  return i;
}

std::string_view trimView(std::string_view text) {
  size_t start = 0;
  while (start < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[start]);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    ++start;
  }
  size_t end = text.size();
  while (end > start) {
    const unsigned char c = static_cast<unsigned char>(text[end - 1]);
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      break;
    }
    --end;
  }
  return text.substr(start, end - start);
}

bool parseQuotedString(std::string_view text, size_t quotePos, std::string& out, size_t& nextPos) {
  if (quotePos >= text.size() || text[quotePos] != '"') {
    return false;
  }
  out.clear();

  for (size_t i = quotePos + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"') {
      nextPos = i + 1;
      return true;
    }
    if (c == '\\') {
      if (i + 1 >= text.size()) {
        return false;
      }
      const char esc = text[++i];
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          out.push_back(esc);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          // Keep parsing lightweight: decode BMP ASCII codepoints, degrade others to '?'.
          if (i + 4 >= text.size()) {
            return false;
          }
          unsigned value = 0;
          for (int nib = 0; nib < 4; ++nib) {
            const char hex = text[i + 1 + static_cast<size_t>(nib)];
            value <<= 4;
            if (hex >= '0' && hex <= '9') {
              value |= static_cast<unsigned>(hex - '0');
            } else if (hex >= 'A' && hex <= 'F') {
              value |= static_cast<unsigned>(hex - 'A' + 10);
            } else if (hex >= 'a' && hex <= 'f') {
              value |= static_cast<unsigned>(hex - 'a' + 10);
            } else {
              return false;
            }
          }
          i += 4;
          out.push_back((value <= 0x7F) ? static_cast<char>(value) : '?');
          break;
        }
        default:
          out.push_back(esc);
          break;
      }
      continue;
    }
    out.push_back(c);
  }

  return false;
}

bool findValueEnd(std::string_view text, size_t start, size_t& endPos) {
  start = skipWs(text, start);
  if (start >= text.size()) {
    return false;
  }

  const char first = text[start];
  if (first == '"') {
    std::string ignored;
    size_t next = 0;
    if (!parseQuotedString(text, start, ignored, next)) {
      return false;
    }
    endPos = next;
    return true;
  }

  if (first == '{' || first == '[') {
    const char open = first;
    const char close = (first == '{') ? '}' : ']';
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = start; i < text.size(); ++i) {
      const char c = text[i];
      if (inString) {
        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == '"') {
          inString = false;
        }
        continue;
      }

      if (c == '"') {
        inString = true;
        continue;
      }
      if (c == open) {
        ++depth;
        continue;
      }
      if (c == close) {
        --depth;
        if (depth == 0) {
          endPos = i + 1;
          return true;
        }
      }
    }
    return false;
  }

  size_t i = start;
  while (i < text.size()) {
    const char c = text[i];
    if (c == ',' || c == '}' || c == ']') {
      break;
    }
    ++i;
  }
  endPos = i;
  return true;
}

bool objectMemberValue(std::string_view objectText, std::string_view key, std::string_view& out) {
  objectText = trimView(objectText);
  if (objectText.size() < 2 || objectText.front() != '{' || objectText.back() != '}') {
    return false;
  }

  size_t i = 1;
  while (i + 1 < objectText.size()) {
    i = skipWs(objectText, i);
    if (i >= objectText.size() - 1 || objectText[i] == '}') {
      break;
    }

    if (objectText[i] != '"') {
      return false;
    }

    std::string memberKey;
    size_t keyEnd = 0;
    if (!parseQuotedString(objectText, i, memberKey, keyEnd)) {
      return false;
    }

    i = skipWs(objectText, keyEnd);
    if (i >= objectText.size() || objectText[i] != ':') {
      return false;
    }

    ++i;
    const size_t valueStart = skipWs(objectText, i);
    size_t valueEnd = 0;
    if (!findValueEnd(objectText, valueStart, valueEnd)) {
      return false;
    }

    if (memberKey == key) {
      out = objectText.substr(valueStart, valueEnd - valueStart);
      return true;
    }

    i = skipWs(objectText, valueEnd);
    if (i < objectText.size() && objectText[i] == ',') {
      ++i;
    }
  }

  return false;
}

bool arrayElementValue(std::string_view arrayText, int index, std::string_view& out) {
  if (index < 0) {
    return false;
  }

  arrayText = trimView(arrayText);
  if (arrayText.size() < 2 || arrayText.front() != '[' || arrayText.back() != ']') {
    return false;
  }

  int current = 0;
  size_t i = 1;
  while (i + 1 < arrayText.size()) {
    i = skipWs(arrayText, i);
    if (i >= arrayText.size() - 1 || arrayText[i] == ']') {
      break;
    }

    size_t valueEnd = 0;
    if (!findValueEnd(arrayText, i, valueEnd)) {
      return false;
    }
    if (current == index) {
      out = arrayText.substr(i, valueEnd - i);
      return true;
    }

    ++current;
    i = skipWs(arrayText, valueEnd);
    if (i < arrayText.size() && arrayText[i] == ',') {
      ++i;
    }
  }

  return false;
}

bool parseStrictDouble(const std::string& text, double& out) {
  std::string trimmed = text;
  const auto notWs = [](unsigned char c) { return c != ' ' && c != '\n' && c != '\r' && c != '\t'; };
  trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), notWs));
  trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), notWs).base(), trimmed.end());
  if (trimmed.empty()) {
    return false;
  }

  errno = 0;
  char* endPtr = nullptr;
  const double value = std::strtod(trimmed.c_str(), &endPtr);
  if (endPtr == nullptr || endPtr == trimmed.c_str() || errno == ERANGE) {
    return false;
  }
  while (*endPtr == ' ' || *endPtr == '\n' || *endPtr == '\r' || *endPtr == '\t') {
    ++endPtr;
  }
  if (*endPtr != '\0') {
    return false;
  }

  out = value;
  return true;
}

bool viewToString(std::string_view valueText, std::string& out) {
  valueText = trimView(valueText);
  if (valueText.empty()) {
    out.clear();
    return false;
  }

  if (valueText.front() == '"') {
    size_t next = 0;
    if (!parseQuotedString(valueText, 0, out, next)) {
      return false;
    }
    return true;
  }

  out.assign(valueText.data(), valueText.size());
  return true;
}

bool viewToInt(std::string_view valueText, int& out) {
  std::string text;
  if (!viewToString(valueText, text)) {
    return false;
  }
  double value = 0.0;
  if (!parseStrictDouble(text, value)) {
    return false;
  }
  if (value < static_cast<double>(std::numeric_limits<int>::min()) ||
      value > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  out = static_cast<int>(std::lround(value));
  return true;
}

bool viewToBool(std::string_view valueText, bool& out) {
  std::string text;
  if (!viewToString(valueText, text)) {
    return false;
  }
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (text == "true" || text == "1" || text == "yes" || text == "on") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0" || text == "no" || text == "off") {
    out = false;
    return true;
  }
  return false;
}

bool viewToDouble(std::string_view valueText, double& out) {
  valueText = trimView(valueText);
  if (valueText.empty()) {
    return false;
  }
  if (valueText.front() == '"') {
    std::string str;
    size_t next = 0;
    if (!parseQuotedString(valueText, 0, str, next)) {
      return false;
    }
    return parseStrictDouble(str, out);
  }
  std::string str(valueText.data(), valueText.size());
  return parseStrictDouble(str, out);
}

bool objectMemberString(std::string_view objectText, const char* key, std::string& out) {
  std::string_view value;
  if (!objectMemberValue(objectText, key, value)) {
    return false;
  }
  return viewToString(value, out);
}

bool objectMemberInt(std::string_view objectText, const char* key, int& out) {
  std::string_view value;
  if (!objectMemberValue(objectText, key, value)) {
    return false;
  }
  return viewToInt(value, out);
}

bool objectMemberBool(std::string_view objectText, const char* key, bool& out) {
  std::string_view value;
  if (!objectMemberValue(objectText, key, value)) {
    return false;
  }
  return viewToBool(value, out);
}

bool objectMemberObject(std::string_view objectText, const char* key, std::string_view& out) {
  if (!objectMemberValue(objectText, key, out)) {
    return false;
  }
  out = trimView(out);
  return out.size() >= 2 && out.front() == '{' && out.back() == '}';
}

bool objectMemberArray(std::string_view objectText, const char* key, std::string_view& out) {
  if (!objectMemberValue(objectText, key, out)) {
    return false;
  }
  out = trimView(out);
  return out.size() >= 2 && out.front() == '[' && out.back() == ']';
}

uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8U) << 8U) | ((g & 0xFCU) << 3U) | (b >> 3U));
}

bool parseHexColor565(const std::string& hex, uint16_t& outColor) {
  if (hex.size() != 7 || hex[0] != '#') {
    return false;
  }
  char* endPtr = nullptr;
  const long value = std::strtol(hex.c_str() + 1, &endPtr, 16);
  if (endPtr == nullptr || *endPtr != '\0' || value < 0 || value > 0xFFFFFF) {
    return false;
  }
  outColor =
      rgbTo565(static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
               static_cast<uint8_t>(value & 0xFF));
  return true;
}

std::string replaceAll(std::string input, const std::string& needle, const std::string& value) {
  if (needle.empty()) {
    return input;
  }
  size_t pos = 0;
  while ((pos = input.find(needle, pos)) != std::string::npos) {
    input.replace(pos, needle.size(), value);
    pos += value.size();
  }
  return input;
}

}  // namespace dsl_json
