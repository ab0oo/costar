#include "dsl/DslParser.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>

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

}  // namespace

namespace dsl {

bool Parser::parseFile(const String& path, Document& out, String* error) {
  File dslFile = SPIFFS.open(path, FILE_READ);
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
      for (JsonObjectConst nodeJson : nodes) {
        Node n;

        const String type = nodeJson["type"] | String("label");
        if (type == "label") {
          n.type = NodeType::kLabel;
        } else if (type == "value_box") {
          n.type = NodeType::kValueBox;
        } else if (type == "progress") {
          n.type = NodeType::kProgress;
        } else if (type == "sparkline") {
          n.type = NodeType::kSparkline;
        } else if (type == "circle") {
          n.type = NodeType::kCircle;
        } else if (type == "hand") {
          n.type = NodeType::kHand;
        } else {
          continue;
        }

        n.x = nodeJson["x"] | 0;
        n.y = nodeJson["y"] | 0;
        n.w = nodeJson["w"] | n.w;
        n.h = nodeJson["h"] | n.h;
        n.font = nodeJson["font"] | n.font;
        n.text = nodeJson["text"] | String();
        n.key = nodeJson["key"] | String();
        n.angleExpr = nodeJson["angle_expr"] | String();
        n.min = nodeJson["min"] | n.min;
        n.max = nodeJson["max"] | n.max;
        n.radius = nodeJson["r"] | n.radius;
        n.length = nodeJson["length"] | n.length;
        n.thickness = nodeJson["thickness"] | n.thickness;

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
