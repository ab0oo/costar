#include "dsl/DslExpr.h"

#include <math.h>

namespace dsl {
namespace {

void skipSpaces(const String& s, int& pos) {
  while (pos < s.length() && (s[pos] == ' ' || s[pos] == '\t')) {
    ++pos;
  }
}

bool parseIdentifier(const String& s, int& pos, String& out) {
  if (pos >= s.length()) {
    return false;
  }
  const char c = s[pos];
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
    return false;
  }
  const int start = pos;
  while (pos < s.length()) {
    const char ch = s[pos];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_')) {
      break;
    }
    ++pos;
  }
  out = s.substring(start, pos);
  return true;
}

bool parseExpr(const String& s, int& pos, const ExprContext& ctx, float& out);

bool parseFunction(const String& name, const String& s, int& pos, const ExprContext& ctx, float& out) {
  skipSpaces(s, pos);
  if (pos >= s.length() || s[pos] != '(') {
    return false;
  }
  ++pos;
  skipSpaces(s, pos);

  float args[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int argc = 0;

  if (pos < s.length() && s[pos] == ')') {
    ++pos;
    argc = 0;
  } else {
    while (true) {
      if (argc >= 4) {
        return false;
      }
      if (!parseExpr(s, pos, ctx, args[argc])) {
        return false;
      }
      ++argc;
      skipSpaces(s, pos);
      if (pos < s.length() && s[pos] == ',') {
        ++pos;
        skipSpaces(s, pos);
        continue;
      }
      if (pos >= s.length() || s[pos] != ')') {
        return false;
      }
      ++pos;
      break;
    }
  }

  const float degToRad = static_cast<float>(M_PI / 180.0);
  const float a = args[0];
  const float b = args[1];
  if (name == "sin") {
    if (argc != 1) return false;
    out = sinf(a * degToRad);
    return true;
  }
  if (name == "cos") {
    if (argc != 1) return false;
    out = cosf(a * degToRad);
    return true;
  }
  if (name == "tan") {
    if (argc != 1) return false;
    out = tanf(a * degToRad);
    return true;
  }
  if (name == "asin") {
    if (argc != 1) return false;
    out = asinf(a) / degToRad;
    return true;
  }
  if (name == "acos") {
    if (argc != 1) return false;
    out = acosf(a) / degToRad;
    return true;
  }
  if (name == "atan") {
    if (argc != 1) return false;
    out = atanf(a) / degToRad;
    return true;
  }
  if (name == "abs") {
    if (argc != 1) return false;
    out = fabsf(a);
    return true;
  }
  if (name == "sqrt") {
    if (argc != 1 || a < 0.0f) return false;
    out = sqrtf(a);
    return true;
  }
  if (name == "floor") {
    if (argc != 1) return false;
    out = floorf(a);
    return true;
  }
  if (name == "ceil") {
    if (argc != 1) return false;
    out = ceilf(a);
    return true;
  }
  if (name == "round") {
    if (argc != 1) return false;
    out = roundf(a);
    return true;
  }
  if (name == "min") {
    if (argc != 2) return false;
    out = fminf(a, b);
    return true;
  }
  if (name == "max") {
    if (argc != 2) return false;
    out = fmaxf(a, b);
    return true;
  }
  if (name == "pow") {
    if (argc != 2) return false;
    out = powf(a, b);
    return true;
  }
  if (name == "rad") {
    if (argc != 1) return false;
    out = a * degToRad;
    return true;
  }
  if (name == "deg") {
    if (argc != 1) return false;
    out = a / degToRad;
    return true;
  }
  if (name == "haversine_m") {
    if (argc != 4) return false;
    const float lat1 = args[0];
    const float lon1 = args[1];
    const float lat2 = args[2];
    const float lon2 = args[3];
    const float dLat = (lat2 - lat1) * degToRad;
    const float dLon = (lon2 - lon1) * degToRad;
    const float s1 = sinf(dLat * 0.5f);
    const float s2 = sinf(dLon * 0.5f);
    float hv = (s1 * s1) + cosf(lat1 * degToRad) * cosf(lat2 * degToRad) * (s2 * s2);
    hv = fmaxf(0.0f, fminf(1.0f, hv));
    const float c = 2.0f * atan2f(sqrtf(hv), sqrtf(fmaxf(0.0f, 1.0f - hv)));
    out = 6371000.0f * c;
    return true;
  }
  if (name == "meters_to_miles") {
    if (argc != 1) return false;
    out = a * 0.000621371f;
    return true;
  }
  if (name == "miles_to_meters") {
    if (argc != 1) return false;
    out = a * 1609.344f;
    return true;
  }

  return false;
}

bool resolveVariable(const ExprContext& ctx, const String& name, float& out) {
  if (name == "pi") {
    out = static_cast<float>(M_PI);
    return true;
  }
  if (ctx.resolver) {
    return ctx.resolver(ctx.ctx, name, out);
  }
  return false;
}

bool parseFactor(const String& s, int& pos, const ExprContext& ctx, float& out) {
  skipSpaces(s, pos);
  if (pos >= s.length()) {
    return false;
  }

  if (s[pos] == '(') {
    ++pos;
    if (!parseExpr(s, pos, ctx, out)) {
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
    if (!parseFactor(s, pos, ctx, out)) {
      return false;
    }
    if (sign == '-') {
      out = -out;
    }
    return true;
  }

  if ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.') {
    const int start = pos;
    while (pos < s.length() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.')) {
      ++pos;
    }
    out = s.substring(start, pos).toFloat();
    return true;
  }

  String ident;
  if (parseIdentifier(s, pos, ident)) {
    skipSpaces(s, pos);
    if (pos < s.length() && s[pos] == '(') {
      return parseFunction(ident, s, pos, ctx, out);
    }
    return resolveVariable(ctx, ident, out);
  }

  return false;
}

bool parseTerm(const String& s, int& pos, const ExprContext& ctx, float& out) {
  if (!parseFactor(s, pos, ctx, out)) {
    return false;
  }

  for (;;) {
    skipSpaces(s, pos);
    if (pos >= s.length() || (s[pos] != '*' && s[pos] != '/' && s[pos] != '%')) {
      break;
    }
    const char op = s[pos++];
    float rhs = 0.0f;
    if (!parseFactor(s, pos, ctx, rhs)) {
      return false;
    }
    if (op == '*') {
      out *= rhs;
    } else if (op == '/') {
      if (fabsf(rhs) < 0.000001f) {
        return false;
      }
      out /= rhs;
    } else {
      if (fabsf(rhs) < 0.000001f) {
        return false;
      }
      out = fmodf(out, rhs);
    }
  }
  return true;
}

bool parseExpr(const String& s, int& pos, const ExprContext& ctx, float& out) {
  if (!parseTerm(s, pos, ctx, out)) {
    return false;
  }

  for (;;) {
    skipSpaces(s, pos);
    if (pos >= s.length() || (s[pos] != '+' && s[pos] != '-')) {
      break;
    }
    const char op = s[pos++];
    float rhs = 0.0f;
    if (!parseTerm(s, pos, ctx, rhs)) {
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

}  // namespace

bool evalExpression(const String& expr, const ExprContext& ctx, float& out) {
  int pos = 0;
  float value = 0.0f;
  if (!parseExpr(expr, pos, ctx, value)) {
    return false;
  }
  skipSpaces(expr, pos);
  if (pos != expr.length()) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace dsl
