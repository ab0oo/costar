#include "dsl/DslParser.h"

#include "dsl/DslExpr.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>

namespace {

uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool parseHexColor565(const String& hex, uint16_t& outColor) {
  if (hex.length() != 7 || hex[0] != '#') {
    return false;
  }

  char* endPtr = nullptr;
  const long value = strtol(hex.substring(1).c_str(), &endPtr, 16);
  if (endPtr == nullptr || *endPtr != '\0' || value < 0 || value > 0xFFFFFF) {
    return false;
  }

  const uint8_t r = static_cast<uint8_t>((value >> 16) & 0xFF);
  const uint8_t g = static_cast<uint8_t>((value >> 8) & 0xFF);
  const uint8_t b = static_cast<uint8_t>(value & 0xFF);
  outColor = rgbTo565(r, g, b);
  return true;
}

uint8_t parseDatum(const String& align, const String& valign) {
  const String h = align;
  const String v = valign;

  const String ha = h.isEmpty() ? "left" : h;
  const String va = v.isEmpty() ? "top" : v;

  if (va == "top") {
    if (ha == "center") return TC_DATUM;
    if (ha == "right") return TR_DATUM;
    return TL_DATUM;
  }
  if (va == "middle") {
    if (ha == "center") return MC_DATUM;
    if (ha == "right") return MR_DATUM;
    return ML_DATUM;
  }
  if (va == "bottom") {
    if (ha == "center") return BC_DATUM;
    if (ha == "right") return BR_DATUM;
    return BL_DATUM;
  }
  if (va == "baseline") {
    if (ha == "center") return C_BASELINE;
    if (ha == "right") return R_BASELINE;
    return L_BASELINE;
  }
  return TL_DATUM;
}

constexpr int kMaxRepeatCount = 512;

struct VarContext {
  const VarContext* parent = nullptr;
  String name;
  float value = 0.0f;
};

bool lookupVar(const VarContext* ctx, const String& name, float& out) {
  for (const VarContext* cur = ctx; cur != nullptr; cur = cur->parent) {
    if (cur->name == name) {
      out = cur->value;
      return true;
    }
  }
  return false;
}

String formatVarValue(float value) {
  const float rounded = roundf(value);
  if (fabsf(value - rounded) < 0.0001f) {
    return String(static_cast<int>(rounded));
  }
  return String(value, 3);
}

String substituteTemplateVars(const String& input, const VarContext* ctx) {
  if (!ctx || input.indexOf("{{") < 0) {
    return input;
  }
  String out;
  out.reserve(input.length());
  int pos = 0;
  for (;;) {
    const int start = input.indexOf("{{", pos);
    if (start < 0) {
      out += input.substring(pos);
      break;
    }
    out += input.substring(pos, start);
    const int end = input.indexOf("}}", start + 2);
    if (end < 0) {
      out += input.substring(start);
      break;
    }
    const String key = input.substring(start + 2, end);
    float value = 0.0f;
    if (lookupVar(ctx, key, value)) {
      out += formatVarValue(value);
    } else {
      out += input.substring(start, end + 2);
    }
    pos = end + 2;
  }
  return out;
}

String substituteExprVars(const String& input, const VarContext* ctx) {
  if (!ctx) {
    return input;
  }
  String out;
  out.reserve(input.length());
  int pos = 0;
  while (pos < input.length()) {
    const char c = input[pos];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
      out += c;
      ++pos;
      continue;
    }
    const int start = pos;
    while (pos < input.length()) {
      const char ch = input[pos];
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_')) {
        break;
      }
      ++pos;
    }
    const String key = input.substring(start, pos);
    float value = 0.0f;
    if (lookupVar(ctx, key, value)) {
      out += formatVarValue(value);
    } else {
      out += key;
    }
  }
  return out;
}

bool resolveVar(void* ctx, const String& name, float& out) {
  return lookupVar(static_cast<const VarContext*>(ctx), name, out);
}

bool evalNumericExpr(const String& expr, const VarContext* ctx, float& out) {
  if (expr.isEmpty()) {
    return false;
  }
  const String templated = substituteTemplateVars(expr, ctx);
  dsl::ExprContext ectx;
  ectx.resolver = &resolveVar;
  ectx.ctx = const_cast<VarContext*>(ctx);
  return dsl::evalExpression(templated, ectx, out);
}

bool parseNumberString(const String& text, float& out) {
  bool hasDigit = false;
  for (int i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if ((c >= '0' && c <= '9')) {
      hasDigit = true;
      break;
    }
  }
  if (!hasDigit) {
    return false;
  }
  out = text.toFloat();
  return true;
}

bool readFloat(JsonVariantConst v, const VarContext* ctx, float& out) {
  if (v.isNull()) {
    return false;
  }
  if (v.is<float>() || v.is<double>() || v.is<long>() || v.is<int>()) {
    out = v.as<float>();
    return true;
  }
  if (v.is<const char*>()) {
    const String text = v.as<String>();
    if (evalNumericExpr(text, ctx, out)) {
      return true;
    }
    return parseNumberString(text, out);
  }
  return false;
}

bool readInt16(JsonVariantConst v, const VarContext* ctx, int16_t& out) {
  float value = 0.0f;
  if (!readFloat(v, ctx, value)) {
    return false;
  }
  out = static_cast<int16_t>(lroundf(value));
  return true;
}

bool readBool(JsonVariantConst v, bool& out) {
  if (v.isNull()) {
    return false;
  }
  if (v.is<bool>()) {
    out = v.as<bool>();
    return true;
  }
  if (v.is<const char*>()) {
    const String text = v.as<String>();
    if (text == "1" || text.equalsIgnoreCase("true") || text.equalsIgnoreCase("yes") ||
        text.equalsIgnoreCase("on")) {
      out = true;
      return true;
    }
    if (text == "0" || text.equalsIgnoreCase("false") || text.equalsIgnoreCase("no") ||
        text.equalsIgnoreCase("off")) {
      out = false;
      return true;
    }
  }
  if (v.is<float>() || v.is<double>() || v.is<long>() || v.is<int>()) {
    out = fabsf(v.as<float>()) > 0.0001f;
    return true;
  }
  return false;
}

void applyNode(JsonObjectConst nodeJson, dsl::Document& out, const VarContext* ctx) {
  dsl::Node n;

  const String type = nodeJson["type"] | String("label");
  if (type == "label") {
    n.type = dsl::NodeType::kLabel;
  } else if (type == "value_box") {
    n.type = dsl::NodeType::kValueBox;
  } else if (type == "progress") {
    n.type = dsl::NodeType::kProgress;
  } else if (type == "sparkline") {
    n.type = dsl::NodeType::kSparkline;
  } else if (type == "arc") {
    n.type = dsl::NodeType::kArc;
  } else if (type == "circle") {
    n.type = dsl::NodeType::kArc;
  } else if (type == "line") {
    n.type = dsl::NodeType::kLine;
  } else if (type == "hand") {
    n.type = dsl::NodeType::kLine;
  } else if (type == "icon") {
    n.type = dsl::NodeType::kIcon;
  } else if (type == "moon_phase") {
    n.type = dsl::NodeType::kMoonPhase;
  } else {
    return;
  }

  readInt16(nodeJson["x"], ctx, n.x);
  readInt16(nodeJson["y"], ctx, n.y);
  readInt16(nodeJson["w"], ctx, n.w);
  readInt16(nodeJson["h"], ctx, n.h);
  readInt16(nodeJson["x2"], ctx, n.x2);
  readInt16(nodeJson["y2"], ctx, n.y2);
  readInt16(nodeJson["r"], ctx, n.radius);
  readInt16(nodeJson["length"], ctx, n.length);
  readInt16(nodeJson["thickness"], ctx, n.thickness);

  if (!nodeJson["font"].isNull()) {
    n.font = nodeJson["font"] | n.font;
  }

  const String text = nodeJson["text"] | String();
  n.text = substituteTemplateVars(text, ctx);
  const String key = nodeJson["key"] | String();
  n.key = substituteTemplateVars(key, ctx);
  const String path = nodeJson["path"] | nodeJson["icon"] | String();
  n.path = substituteTemplateVars(path, ctx);
  const String angleExpr = nodeJson["angle_expr"] | String();
  n.angleExpr = substituteExprVars(substituteTemplateVars(angleExpr, ctx), ctx);

  const String align = nodeJson["align"] | String();
  const String valign = nodeJson["valign"] | String();
  n.datum = parseDatum(align, valign);
  readBool(nodeJson["wrap"], n.wrap);
  readInt16(nodeJson["line_height"], ctx, n.lineHeight);
  readInt16(nodeJson["max_lines"], ctx, n.maxLines);
  const String overflow = nodeJson["overflow"] | String();
  if (overflow.equalsIgnoreCase("ellipsis")) {
    n.overflow = dsl::OverflowMode::kEllipsis;
  } else {
    n.overflow = dsl::OverflowMode::kClip;
  }

  float fval = 0.0f;
  if (readFloat(nodeJson["min"], ctx, fval)) {
    n.min = fval;
  }
  if (readFloat(nodeJson["max"], ctx, fval)) {
    n.max = fval;
  }
  if (readFloat(nodeJson["start_deg"], ctx, fval)) {
    n.startDeg = fval;
  }
  if (readFloat(nodeJson["end_deg"], ctx, fval)) {
    n.endDeg = fval;
  }

  const String colorHex = nodeJson["color"] | String("#FFFFFF");
  if (!parseHexColor565(colorHex, n.color565)) {
    n.color565 = 0xFFFF;
  }

  const String bgHex = nodeJson["bg"] | String("#101010");
  if (!parseHexColor565(bgHex, n.bg565)) {
    n.bg565 = 0x0000;
  }

  out.nodes.push_back(n);
}

void applyNodes(JsonArrayConst nodes, dsl::Document& out, const VarContext* ctx) {
  if (nodes.isNull()) {
    return;
  }
  for (JsonObjectConst nodeJson : nodes) {
    const String type = nodeJson["type"] | String("label");
    if (type == "repeat") {
      int count = nodeJson["count"] | 0;
      if (!nodeJson["times"].isNull()) {
        count = nodeJson["times"] | count;
      }
      if (count <= 0) {
        continue;
      }
      if (count > kMaxRepeatCount) {
        count = kMaxRepeatCount;
      }
      float start = 0.0f;
      float step = 1.0f;
      readFloat(nodeJson["start"], ctx, start);
      readFloat(nodeJson["step"], ctx, step);
      const String var = nodeJson["var"] | String("i");

      JsonArrayConst childNodes = nodeJson["nodes"];
      JsonObjectConst singleNode = nodeJson["node"];
      for (int i = 0; i < count; ++i) {
        VarContext local;
        local.parent = ctx;
        local.name = var;
        local.value = start + static_cast<float>(i) * step;
        if (!childNodes.isNull()) {
          applyNodes(childNodes, out, &local);
        } else if (!singleNode.isNull()) {
          applyNode(singleNode, out, &local);
        }
      }
      continue;
    }

    applyNode(nodeJson, out, ctx);
  }
}

}  // namespace

namespace dsl {

bool Parser::parseFile(const String& path, Document& out, String* error) {
  fs::File dslFile = LittleFS.open(path, FILE_READ);
  if (!dslFile || dslFile.isDirectory()) {
    if (error != nullptr) {
      *error = "dsl file missing";
    }
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, dslFile);
  dslFile.close();

  if (parseError) {
    if (error != nullptr) {
      *error = "dsl parse failed";
    }
    return false;
  }

  const int version = doc["version"] | 1;
  if (version != 1) {
    if (error != nullptr) {
      *error = "unsupported dsl version";
    }
    return false;
  }

  out = Document();
  out.debug = doc["debug"] | out.debug;

  const JsonObjectConst data = doc["data"];
  if (!data.isNull()) {
    out.source = data["source"] | out.source;
    out.url = data["url"] | String();
    const JsonObjectConst headers = data["headers"];
    if (!headers.isNull()) {
      for (JsonPairConst p : headers) {
        const String key = String(p.key().c_str());
        String value;
        if (p.value().is<const char*>()) {
          value = p.value().as<String>();
        } else if (p.value().is<float>() || p.value().is<double>() ||
                   p.value().is<long>() || p.value().is<int>() || p.value().is<bool>()) {
          value = p.value().as<String>();
        }
        if (!key.isEmpty() && !value.isEmpty()) {
          out.headers[key] = value;
        }
      }
    }
    out.debug = data["debug"] | out.debug;
    out.pollMs = data["poll_ms"] | out.pollMs;

    const JsonObjectConst fields = data["fields"];
    if (!fields.isNull()) {
      for (JsonPairConst p : fields) {
        FieldSpec spec;

        if (p.value().is<const char*>()) {
          spec.path = p.value().as<String>();
        } else if (p.value().is<JsonObjectConst>()) {
          const JsonObjectConst obj = p.value().as<JsonObjectConst>();
          spec.path = obj["path"] | String();

          const JsonObjectConst fmt = obj["format"];
          if (!fmt.isNull()) {
            if (!fmt["round"].isNull()) {
              spec.format.roundDigits = fmt["round"];
            }
            spec.format.prefix = fmt["prefix"] | String();
            spec.format.suffix = fmt["suffix"] | String();
            spec.format.unit = fmt["unit"] | String();
            spec.format.locale = fmt["locale"] | String("en-US");
            spec.format.tz = fmt["tz"] | String();
            spec.format.timeFormat = fmt["time_format"] | String("%Y-%m-%d %H:%M");
          }
        }

        if (!spec.path.isEmpty()) {
          out.fields[String(p.key().c_str())] = spec;
        }
      }
    }
  }

  const JsonObjectConst ui = doc["ui"];
  if (!ui.isNull()) {
    out.title = ui["title"] | out.title;
    out.debug = ui["debug"] | out.debug;

    const JsonArrayConst labels = ui["labels"];
    if (!labels.isNull()) {
      for (JsonObjectConst nodeJson : labels) {
        Node n;
        n.type = NodeType::kLabel;
        n.x = nodeJson["x"] | 0;
        n.y = nodeJson["y"] | 0;
        n.font = nodeJson["font"] | 2;
        n.text = nodeJson["text"] | String();

        const String colorHex = nodeJson["color"] | String("#FFFFFF");
        if (!parseHexColor565(colorHex, n.color565)) {
          n.color565 = 0xFFFF;
        }
        out.nodes.push_back(n);
      }
    }

    const JsonArrayConst nodes = ui["nodes"];
    if (!nodes.isNull()) {
      applyNodes(nodes, out, nullptr);
    }
  }

  if (out.nodes.empty()) {
    Node fallback;
    fallback.type = NodeType::kLabel;
    fallback.text = "DSL widget loaded";
    fallback.x = 8;
    fallback.y = 30;
    fallback.font = 2;
    fallback.color565 = 0xFFFF;
    out.nodes.push_back(fallback);
  }

  return true;
}

}  // namespace dsl
