#include "widgets/DslWidget.h"

#include <math.h>

bool DslWidget::getNumeric(const String& key, float& out) const {
  auto it = values_.find(key);
  if (it == values_.end()) {
    return false;
  }

  const String& text = it->second;
  bool hasDigit = false;
  String filtered;
  filtered.reserve(text.length());
  for (int i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+') {
      filtered += c;
      if (c >= '0' && c <= '9') {
        hasDigit = true;
      }
    }
  }

  if (!hasDigit) {
    return false;
  }

  out = filtered.toFloat();
  return true;
}

void DslWidget::skipSpaces(const String& s, int& pos) const {
  while (pos < s.length() && (s[pos] == ' ' || s[pos] == '\t')) {
    ++pos;
  }
}

bool DslWidget::parseFactor(const String& s, int& pos, float& out) const {
  skipSpaces(s, pos);
  if (pos >= s.length()) {
    return false;
  }

  if (s[pos] == '(') {
    ++pos;
    if (!parseExpr(s, pos, out)) {
      return false;
    }
    skipSpaces(s, pos);
    if (pos >= s.length() || s[pos] != ')') {
      return false;
    }
    ++pos;
    return true;
  }

  if (s[pos] == '+' || s[pos] == '-') {
    const char sign = s[pos++];
    if (!parseFactor(s, pos, out)) {
      return false;
    }
    if (sign == '-') {
      out = -out;
    }
    return true;
  }

  if ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.') {
    int start = pos;
    while (pos < s.length() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.')) {
      ++pos;
    }
    out = s.substring(start, pos).toFloat();
    return true;
  }

  if ((s[pos] >= 'a' && s[pos] <= 'z') || (s[pos] >= 'A' && s[pos] <= 'Z') || s[pos] == '_') {
    int start = pos;
    while (pos < s.length() && ((s[pos] >= 'a' && s[pos] <= 'z') || (s[pos] >= 'A' && s[pos] <= 'Z') ||
                                (s[pos] >= '0' && s[pos] <= '9') || s[pos] == '_')) {
      ++pos;
    }
    const String key = s.substring(start, pos);
    return getNumeric(key, out);
  }

  return false;
}

bool DslWidget::parseTerm(const String& s, int& pos, float& out) const {
  if (!parseFactor(s, pos, out)) {
    return false;
  }

  for (;;) {
    skipSpaces(s, pos);
    if (pos >= s.length() || (s[pos] != '*' && s[pos] != '/')) {
      break;
    }
    const char op = s[pos++];
    float rhs = 0.0f;
    if (!parseFactor(s, pos, rhs)) {
      return false;
    }
    if (op == '*') {
      out *= rhs;
    } else {
      if (fabsf(rhs) < 0.000001f) {
        return false;
      }
      out /= rhs;
    }
  }
  return true;
}

bool DslWidget::parseExpr(const String& s, int& pos, float& out) const {
  if (!parseTerm(s, pos, out)) {
    return false;
  }

  for (;;) {
    skipSpaces(s, pos);
    if (pos >= s.length() || (s[pos] != '+' && s[pos] != '-')) {
      break;
    }
    const char op = s[pos++];
    float rhs = 0.0f;
    if (!parseTerm(s, pos, rhs)) {
      return false;
    }
    if (op == '+') {
      out += rhs;
    } else {
      out -= rhs;
    }
  }
  return true;
}

bool DslWidget::evaluateAngleExpr(const String& expr, float& outDegrees) const {
  const String e = bindRuntimeTemplate(expr);
  int pos = 0;
  float value = 0.0f;
  if (!parseExpr(e, pos, value)) {
    return false;
  }
  skipSpaces(e, pos);
  if (pos != e.length()) {
    return false;
  }
  outDegrees = value;
  return true;
}
