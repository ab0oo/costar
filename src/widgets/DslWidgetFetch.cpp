#include "widgets/DslWidget.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <math.h>
#include <time.h>

#include "RuntimeGeo.h"
#include "RuntimeSettings.h"

namespace {
bool inferOffsetFromTimezone(const String& tz, int& outMinutes) {
  if (tz == "America/Los_Angeles") {
    outMinutes = -8 * 60;
    return true;
  }
  if (tz == "America/Denver") {
    outMinutes = -7 * 60;
    return true;
  }
  if (tz == "America/Chicago") {
    outMinutes = -6 * 60;
    return true;
  }
  if (tz == "America/New_York") {
    outMinutes = -5 * 60;
    return true;
  }
  if (tz == "UTC" || tz == "Etc/UTC") {
    outMinutes = 0;
    return true;
  }
  return false;
}

String clipText(const String& text, size_t maxLen = 96) {
  if (text.length() <= static_cast<int>(maxLen)) {
    return text;
  }
  if (maxLen < 8) {
    return text.substring(0, static_cast<int>(maxLen));
  }
  const int head = static_cast<int>(maxLen / 2) - 2;
  const int tail = static_cast<int>(maxLen) - head - 3;
  return text.substring(0, head) + "..." + text.substring(text.length() - tail);
}

String describeTransportStage(const HttpFetchMeta& meta) {
  if (meta.statusCode == -2 || meta.transportReason == "tls-preflight-low-largest-block") {
    return "request-not-attempted (tls-preflight)";
  }
  if (meta.statusCode == -3 || meta.transportReason == "transport-cooldown") {
    return "request-skipped (transport-cooldown)";
  }
  if (meta.transportReason.indexOf("begin failed") >= 0) {
    return "request-not-attempted (http-begin)";
  }
  if (meta.transportReason.indexOf("transport gate timeout") >= 0) {
    return "request-not-attempted (transport-gate-timeout)";
  }
  return "transport-failure (no-http-status)";
}
}  // namespace

std::map<String, String> DslWidget::resolveTapHeaders(const dsl::TouchAction& action) const {
  std::map<String, String> headers;
  for (const auto& kv : action.headers) {
    String name = kv.first;
    name.trim();
    if (name.isEmpty()) {
      continue;
    }
    String value = bindTemplate(kv.second);
    value.trim();
    if (!value.isEmpty()) {
      headers[name] = value;
    }
  }

  for (const auto& kv : config_.settings) {
    if (!kv.first.startsWith("tap_header_")) {
      continue;
    }
    String name = kv.first.substring(String("tap_header_").length());
    name.trim();
    if (name.isEmpty()) {
      continue;
    }
    name.replace("_", "-");
    String value = bindTemplate(kv.second);
    value.trim();
    if (!value.isEmpty()) {
      headers[name] = value;
    }
  }
  return headers;
}

bool DslWidget::executeTapAction(String& errorOut) {
  dsl::TouchAction action;
  if (hasPendingTouchAction_) {
    action = pendingTouchAction_;
  } else if (!dsl_.onTouch.action.isEmpty()) {
    action = dsl_.onTouch;
  } else {
    action = buildLegacyTouchAction();
  }

  if (!actionIsHttp(action)) {
    return false;
  }

  String url = action.url;
  if (url.isEmpty()) {
    errorOut = "tap_url missing";
    return false;
  }
  url = bindTemplate(url);
  if (url.isEmpty()) {
    errorOut = "tap_url empty";
    return false;
  }

  String method = action.method;
  if (method.isEmpty()) {
    method = "POST";
  }
  method.trim();
  method.toUpperCase();
  if (method.isEmpty()) {
    method = "POST";
  }

  String body = action.body;
  body = bindTemplate(body);

  String contentType = action.contentType;
  if (contentType.isEmpty()) {
    contentType = "application/json";
  }
  contentType = bindTemplate(contentType);
  contentType.trim();
  if (contentType.isEmpty()) {
    contentType = "application/json";
  }

  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "WiFi disconnected";
    return false;
  }

  HTTPClient http;
  WiFiClientSecure secureClient;
  bool begun = false;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(url);
  }
  if (!begun) {
    errorOut = "HTTP begin failed";
    return false;
  }

  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setConnectTimeout(4500);
  http.setTimeout(6500);
  http.useHTTP10(true);
  http.setReuse(false);
  http.addHeader("User-Agent", "CoStar-ESP32/1.0");
  const std::map<String, String> headers = resolveTapHeaders(action);
  for (const auto& kv : headers) {
    if (kv.first.isEmpty() || kv.second.isEmpty()) {
      continue;
    }
    http.addHeader(kv.first, kv.second);
  }
  if (!body.isEmpty()) {
    http.addHeader("Content-Type", contentType);
  }

  int status = 0;
  if (method == "GET") {
    status = http.GET();
  } else if (method == "POST") {
    status = http.POST(body);
  } else if (method == "PUT") {
    status = http.PUT(body);
  } else if (method == "PATCH") {
    status = http.sendRequest("PATCH", body);
  } else if (method == "DELETE") {
    status = http.sendRequest("DELETE", body);
  } else {
    status = http.sendRequest(method.c_str(), body);
  }

  if (status < 200 || status >= 300) {
    const String response = http.getString();
    errorOut = "status=" + String(status) + " body='" + clipText(response, 72) + "'";
    http.end();
    return false;
  }

  http.end();
  return true;
}

bool DslWidget::buildLocalTimeDoc(JsonDocument& outDoc, String& error) const {
  const time_t nowUtc = time(nullptr);
  if (nowUtc < 946684800) {
    error = "time unavailable";
    return false;
  }

  time_t localEpoch = nowUtc;
  int offsetMinutes = 0;
  bool haveOffset = false;
  if (RuntimeGeo::hasUtcOffset) {
    offsetMinutes = RuntimeGeo::utcOffsetMinutes;
    haveOffset = true;
  } else if (inferOffsetFromTimezone(RuntimeGeo::timezone, offsetMinutes)) {
    haveOffset = true;
  }
  if (haveOffset) {
    localEpoch += static_cast<time_t>(offsetMinutes) * 60;
  }

  struct tm tmUtc;
  struct tm tmLocal;
  gmtime_r(&nowUtc, &tmUtc);
  gmtime_r(&localEpoch, &tmLocal);

  char timeBuf[16];
  char time12Buf[16];
  char dateBuf[16];
  char isoBuf[24];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", tmLocal.tm_hour, tmLocal.tm_min,
           tmLocal.tm_sec);
  int h12 = tmLocal.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  snprintf(time12Buf, sizeof(time12Buf), "%02d:%02d:%02d %s", h12, tmLocal.tm_min, tmLocal.tm_sec,
           tmLocal.tm_hour >= 12 ? "PM" : "AM");
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", tmLocal.tm_year + 1900, tmLocal.tm_mon + 1,
           tmLocal.tm_mday);
  snprintf(isoBuf, sizeof(isoBuf), "%04d-%02d-%02dT%02d:%02d", tmLocal.tm_year + 1900,
           tmLocal.tm_mon + 1, tmLocal.tm_mday, tmLocal.tm_hour, tmLocal.tm_min);

  outDoc["time"] = RuntimeSettings::use24HourClock ? String(timeBuf) : String(time12Buf);
  outDoc["time_24"] = String(timeBuf);
  outDoc["time_12"] = String(time12Buf);
  outDoc["date"] = String(dateBuf);
  outDoc["iso_local"] = String(isoBuf);
  outDoc["hour"] = tmLocal.tm_hour;
  outDoc["minute"] = tmLocal.tm_min;
  outDoc["second"] = tmLocal.tm_sec;
  outDoc["millis"] = millis() % 1000;
  outDoc["epoch"] = static_cast<long>(nowUtc);
  outDoc["tz"] = RuntimeGeo::timezone;
  outDoc["offset_min"] = offsetMinutes;
  outDoc["offset_known"] = haveOffset;
  return true;
}

float DslWidget::distanceKm(float lat1, float lon1, float lat2, float lon2) const {
  constexpr float kDegToRad = 3.14159265f / 180.0f;
  constexpr float kEarthRadiusKm = 6371.0f;
  const float dLat = (lat2 - lat1) * kDegToRad;
  const float dLon = (lon2 - lon1) * kDegToRad;
  const float a = sinf(dLat * 0.5f) * sinf(dLat * 0.5f) +
                  cosf(lat1 * kDegToRad) * cosf(lat2 * kDegToRad) *
                      sinf(dLon * 0.5f) * sinf(dLon * 0.5f);
  const float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
  return kEarthRadiusKm * c;
}

bool DslWidget::buildAdsbNearestDoc(const JsonDocument& rawDoc, JsonDocument& outDoc,
                                    String& error) const {
  JsonArrayConst ac = rawDoc["ac"].as<JsonArrayConst>();
  if (ac.isNull()) {
    ac = rawDoc.as<JsonArrayConst>();
  }
  if (ac.isNull()) {
    error = "adsb response missing aircraft list";
    return false;
  }

  struct Row {
    float km = 1e9f;
    String flight;
    String distanceText;
    String altText;
    String type;
    String dest;
    String line;
  };
  std::vector<Row> rows;
  rows.reserve(ac.size());
  auto clipField = [](const String& in, size_t maxLen) -> String {
    if (in.length() <= static_cast<int>(maxLen)) {
      return in;
    }
    if (maxLen <= 1) {
      return in.substring(0, static_cast<int>(maxLen));
    }
    return String(in.substring(0, static_cast<int>(maxLen - 1)) + ".");
  };

  for (JsonVariantConst v : ac) {
    if (!v.is<JsonObjectConst>()) {
      continue;
    }
    JsonObjectConst obj = v.as<JsonObjectConst>();

    if (obj["lat"].isNull() || obj["lon"].isNull()) {
      continue;
    }
    const float lat = obj["lat"].as<float>();
    const float lon = obj["lon"].as<float>();

    Row row;
    if (!obj["dst"].isNull()) {
      // adsb.lol provides "dst" (distance from query point) in nautical miles.
      row.km = obj["dst"].as<float>() * 1.852f;
    } else {
      row.km = distanceKm(RuntimeGeo::latitude, RuntimeGeo::longitude, lat, lon);
    }

    String flight = obj["flight"] | String();
    flight.trim();
    if (flight.isEmpty()) {
      flight = obj["callsign"] | String();
      flight.trim();
    }
    if (flight.isEmpty()) {
      flight = obj["hex"] | String("?");
    }
    row.flight = clipField(flight, 8);

    String type = obj["t"] | String();
    type.trim();
    if (type.isEmpty()) {
      type = obj["type"] | String();
      type.trim();
    }
    row.type = clipField(type.isEmpty() ? String("?") : type, 5);

    String dest = obj["destination"] | String();
    dest.trim();
    if (dest.isEmpty()) {
      dest = obj["route"] | String();
      dest.trim();
    }
    if (dest.isEmpty() && obj["to"].is<const char*>()) {
      dest = obj["to"].as<String>();
      dest.trim();
    }
    row.dest = clipField(dest.isEmpty() ? String("?") : dest, 8);

    String altText = "?";
    if (!obj["alt_baro"].isNull()) {
      if (obj["alt_baro"].is<float>() || obj["alt_baro"].is<long>() || obj["alt_baro"].is<int>()) {
        altText = String(obj["alt_baro"].as<int>()) + "ft";
      } else {
        const String altRaw = obj["alt_baro"].as<String>();
        altText = altRaw.equalsIgnoreCase("ground") ? String("GND") : altRaw;
      }
    } else if (!obj["altitude"].isNull()) {
      altText = String(obj["altitude"].as<int>()) + "ft";
    }
    row.altText = altText;

    const float dist = RuntimeSettings::useMiles ? (row.km * 0.621371f) : row.km;
    const String unit = RuntimeSettings::useMiles ? "mi" : "km";
    row.distanceText = String(dist, 1) + unit;

    row.line = row.flight + " " + row.distanceText + " " + row.altText + " " + row.type + "->" +
               row.dest;
    rows.push_back(row);
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.km < b.km; });
  if (rows.size() > 5) {
    rows.resize(5);
  }

  outDoc["count"] = static_cast<int>(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    const String idx = String(i + 1);
    outDoc["row" + idx] = rows[i].line;
    outDoc["flight" + idx] = rows[i].flight;
    outDoc["distance" + idx] = rows[i].distanceText;
    outDoc["altitude" + idx] = rows[i].altText;
    outDoc["type" + idx] = rows[i].type;
    outDoc["destination" + idx] = rows[i].dest;
  }
  for (int i = static_cast<int>(rows.size()) + 1; i <= 5; ++i) {
    const String idx = String(i);
    outDoc["row" + idx] = String();
    outDoc["flight" + idx] = String();
    outDoc["distance" + idx] = String();
    outDoc["altitude" + idx] = String();
    outDoc["type" + idx] = String();
    outDoc["destination" + idx] = String();
  }
  return true;
}

bool DslWidget::update(uint32_t nowMs) {
  if (!dslLoaded_) {
    return false;
  }
  if (modalVisible_ && modalDismissAtMs_ != 0 &&
      static_cast<int32_t>(nowMs - modalDismissAtMs_) >= 0) {
    modalVisible_ = false;
    activeModalId_ = "";
    modalDismissAtMs_ = 0;
    return true;
  }
  if (firstFetch_ && startDelayMs_ > 0 &&
      static_cast<int32_t>(nowMs - firstFetchNotBeforeMs_) < 0) {
    return false;
  }

  if (tapActionPending_) {
    String actionError;
    if (!executeTapAction(actionError)) {
      status_ = "tap err";
      if (dsl_.debug) {
        Serial.printf("[%s] [%s] TAP err=%s\n", widgetName().c_str(), logTimestamp().c_str(),
                      clipText(actionError, 120).c_str());
      }
    } else {
      status_ = "ok";
      if (dsl_.debug) {
        Serial.printf("[%s] [%s] TAP ok\n", widgetName().c_str(), logTimestamp().c_str());
      }
    }
    tapActionPending_ = false;
    hasPendingTouchAction_ = false;
    forceFetchNow_ = true;
  }

  if (!forceFetchNow_) {
    if (dsl_.source == "adsb_nearest") {
      if (adsbBackoffUntilMs_ != 0 && static_cast<int32_t>(nowMs - adsbBackoffUntilMs_) < 0) {
        return false;
      }
      if (nextFetchMs_ == 0) {
        nextFetchMs_ = nowMs;
      }
      if (!firstFetch_ && static_cast<int32_t>(nowMs - nextFetchMs_) < 0) {
        return false;
      }
    } else if (dsl_.source == "http" && httpBackoffUntilMs_ != 0 &&
               static_cast<int32_t>(nowMs - httpBackoffUntilMs_) < 0) {
      return false;
    } else if (nowMs - lastFetchMs_ < dsl_.pollMs) {
      if (!firstFetch_) {
        return false;
      }
    }
  }
  lastFetchMs_ = nowMs;
  if (dsl_.source == "adsb_nearest") {
    nextFetchMs_ = nowMs + dsl_.pollMs + computeAdsbJitterMs(dsl_.pollMs);
  }
  firstFetch_ = false;
  forceFetchNow_ = false;

  JsonDocument doc;
  String error;
  HttpFetchMeta fetchMeta;

  if (dsl_.source == "local_time") {
    if (!buildLocalTimeDoc(doc, error)) {
      if (dsl_.debug) {
        Serial.printf("[%s] - [%s] - DSL local_time error: %s\n", widgetName().c_str(),
                      logTimestamp().c_str(), error.c_str());
      }
    }
  } else if (dsl_.source == "adsb_nearest") {
    const String resolvedUrl = bindRuntimeTemplate(dsl_.url);
    String altTransportUrl = resolvedUrl;
    if (altTransportUrl.startsWith("https://")) {
      altTransportUrl.replace("https://", "http://");
    }
    String radiusNm;
    if (RuntimeSettings::adsbRadiusNm > 0) {
      radiusNm = String(RuntimeSettings::adsbRadiusNm);
    } else if (config_.settings.count("radius_nm")) {
      radiusNm = config_.settings.at("radius_nm");
    } else {
      radiusNm = "40";
    }
    const String fallbackUrlHttps = "https://api.airplanes.live/v2/point/" +
                                    String(RuntimeGeo::latitude, 4) + "/" +
                                    String(RuntimeGeo::longitude, 4) + "/" + radiusNm;
    const String fallbackUrlHttp = "http://api.airplanes.live/v2/point/" +
                                   String(RuntimeGeo::latitude, 4) + "/" +
                                   String(RuntimeGeo::longitude, 4) + "/" + radiusNm;
    if (dsl_.debug) {
      Serial.printf("[%s] [%s] URL %s\n", widgetName().c_str(), logTimestamp().c_str(),
                    clipText(resolvedUrl, 88).c_str());
    }
    JsonDocument rawDoc;
    bool gotRaw = false;
    bool fetchedFromFallback = false;

    if (resolvedUrl.isEmpty()) {
      error = "resolved URL empty";
    } else if (http_.get(resolvedUrl, rawDoc, &error, &fetchMeta)) {
      gotRaw = true;
    } else {
      if (altTransportUrl != resolvedUrl) {
        if (dsl_.debug) {
          Serial.printf("[%s] [%s] ADSB retry http %s\n",
                        widgetName().c_str(), logTimestamp().c_str(), altTransportUrl.c_str());
        }
        String altErr;
        HttpFetchMeta altMeta;
        JsonDocument altDoc;
        if (http_.get(altTransportUrl, altDoc, &altErr, &altMeta)) {
          rawDoc = altDoc;
          fetchMeta = altMeta;
          error = "";
          gotRaw = true;
        } else {
          error = altErr;
          fetchMeta = altMeta;
        }
      }
    }

    if (!gotRaw) {
      if (dsl_.debug) {
        Serial.printf("[%s] [%s] ADSB fallback %s\n", widgetName().c_str(),
                      logTimestamp().c_str(), clipText(fallbackUrlHttps, 72).c_str());
      }
      String fallbackError;
      HttpFetchMeta fallbackMeta;
      JsonDocument fallbackDoc;
      if (http_.get(fallbackUrlHttps, fallbackDoc, &fallbackError, &fallbackMeta) ||
          http_.get(fallbackUrlHttp, fallbackDoc, &fallbackError, &fallbackMeta)) {
        rawDoc = fallbackDoc;
        fetchMeta = fallbackMeta;
        error = "";
        gotRaw = true;
        fetchedFromFallback = true;
      } else {
        error = "primary=" + error + ", fallback=" + fallbackError;
        fetchMeta = fallbackMeta;
      }
    }

    if (gotRaw) {
      logHttpFetchResult(fetchMeta.statusCode, fetchMeta.contentLengthBytes);
      if (!buildAdsbNearestDoc(rawDoc, doc, error) && !fetchedFromFallback) {
        if (dsl_.debug) {
          Serial.printf("[%s] [%s] ADSB parse err=%s\n", widgetName().c_str(),
                        logTimestamp().c_str(), clipText(error, 86).c_str());
        }
        String fallbackError;
        HttpFetchMeta fallbackMeta;
        JsonDocument fallbackDoc;
        if ((http_.get(fallbackUrlHttps, fallbackDoc, &fallbackError, &fallbackMeta) ||
             http_.get(fallbackUrlHttp, fallbackDoc, &fallbackError, &fallbackMeta)) &&
            buildAdsbNearestDoc(fallbackDoc, doc, fallbackError)) {
          fetchMeta = fallbackMeta;
          error = "";
        } else {
          error = "primary_parse=" + error + ", fallback=" + fallbackError;
          fetchMeta = fallbackMeta;
        }
      }
    }
    if (!error.isEmpty()) {
      logHttpFetchResult(fetchMeta.statusCode, fetchMeta.contentLengthBytes);
      if (dsl_.debug) {
        if (fetchMeta.statusCode <= 0) {
          Serial.printf("[%s] [%s] ADSB %s code=%d reason='%s' elapsed=%lums\n",
                        widgetName().c_str(), logTimestamp().c_str(),
                        describeTransportStage(fetchMeta).c_str(), fetchMeta.statusCode,
                        fetchMeta.transportReason.c_str(),
                        static_cast<unsigned long>(fetchMeta.elapsedMs));
        }
        Serial.printf(
            "[%s] [%s] ADSB err=%s status=%d bytes=%u ctype='%s'\n", widgetName().c_str(),
            logTimestamp().c_str(), clipText(error, 140).c_str(), fetchMeta.statusCode,
            static_cast<unsigned>(fetchMeta.payloadBytes), fetchMeta.contentType.c_str());
      }
    }
  } else if (dsl_.source == "http") {
    const String resolvedUrl = bindRuntimeTemplate(dsl_.url);
    const std::map<String, String> resolvedHeaders = resolveHttpHeaders();
    const std::map<String, String>* headersPtr =
        resolvedHeaders.empty() ? nullptr : &resolvedHeaders;
    if (dsl_.debug) {
      Serial.printf("[%s] [%s] URL %s\n", widgetName().c_str(), logTimestamp().c_str(),
                    clipText(resolvedUrl, 88).c_str());
      if (!resolvedHeaders.empty()) {
        Serial.printf("[%s] [%s] HTTP headers=%u\n", widgetName().c_str(),
                      logTimestamp().c_str(), static_cast<unsigned>(resolvedHeaders.size()));
      }
    }
    if (resolvedUrl.isEmpty()) {
      error = "resolved URL empty";
    } else if (!http_.get(resolvedUrl, doc, &error, &fetchMeta, headersPtr)) {
      const bool retryForEmptyPayload = error.startsWith("Empty payload");
      const bool retryForTransport = fetchMeta.statusCode <= 0 && fetchMeta.statusCode != -2 &&
                                     fetchMeta.statusCode != -3;
      if (retryForEmptyPayload || retryForTransport) {
        if (dsl_.debug && retryForTransport) {
          Serial.printf("[%s] [%s] DSL retry after transport failure code=%d reason='%s'\n",
                        widgetName().c_str(), logTimestamp().c_str(), fetchMeta.statusCode,
                        fetchMeta.transportReason.c_str());
        }
        delay(retryForTransport ? 140 : 40);
        JsonDocument retryDoc;
        String retryError;
        HttpFetchMeta retryMeta;
        if (http_.get(resolvedUrl, retryDoc, &retryError, &retryMeta, headersPtr)) {
          doc = retryDoc;
          fetchMeta = retryMeta;
          error = "";
        } else {
          error = retryError;
          fetchMeta = retryMeta;
        }
      }
    }

    if (!error.isEmpty()) {
      logHttpFetchResult(fetchMeta.statusCode, fetchMeta.contentLengthBytes);
      if (fetchMeta.statusCode <= 0) {
        Serial.printf("[%s] [%s] DSL %s url=%s code=%d reason='%s' elapsed=%lums\n",
                      widgetName().c_str(), logTimestamp().c_str(),
                      describeTransportStage(fetchMeta).c_str(), clipText(resolvedUrl, 96).c_str(),
                      fetchMeta.statusCode, fetchMeta.transportReason.c_str(),
                      static_cast<unsigned long>(fetchMeta.elapsedMs));
      }
      if (dsl_.debug) {
        if (fetchMeta.statusCode <= 0) {
          Serial.printf("[%s] [%s] DSL %s code=%d reason='%s' elapsed=%lums\n",
                        widgetName().c_str(), logTimestamp().c_str(),
                        describeTransportStage(fetchMeta).c_str(), fetchMeta.statusCode,
                        fetchMeta.transportReason.c_str(),
                        static_cast<unsigned long>(fetchMeta.elapsedMs));
        } else if (fetchMeta.statusCode == 429 || fetchMeta.statusCode == 503) {
          Serial.printf("[%s] [%s] DSL server throttle status=%d retry-after='%s'\n",
                        widgetName().c_str(), logTimestamp().c_str(), fetchMeta.statusCode,
                        fetchMeta.retryAfter.c_str());
        }
        Serial.printf(
            "[%s] [%s] DSL err=%s status=%d bytes=%u ctype='%s'\n", widgetName().c_str(),
            logTimestamp().c_str(), clipText(error, 140).c_str(), fetchMeta.statusCode,
            static_cast<unsigned>(fetchMeta.payloadBytes), fetchMeta.contentType.c_str());
      }
    } else {
      logHttpFetchResult(fetchMeta.statusCode, fetchMeta.contentLengthBytes);
      if (dsl_.debug) {
        Serial.printf("[%s] [%s] DSL ok url=%s bytes=%u\n", widgetName().c_str(),
                      logTimestamp().c_str(), clipText(resolvedUrl, 70).c_str(),
                      static_cast<unsigned>(fetchMeta.payloadBytes));
      }
    }
  } else {
    error = "unsupported source: " + dsl_.source;
    if (dsl_.debug) {
      Serial.printf("[%s] - [%s] - DSL config error: %s\n", widgetName().c_str(),
                    logTimestamp().c_str(), error.c_str());
    }
  }

  if (!error.isEmpty()) {
    if (dsl_.source == "adsb_nearest") {
      adsbFailureStreak_ = static_cast<uint8_t>(adsbFailureStreak_ < 7 ? adsbFailureStreak_ + 1 : 7);
      uint32_t backoffMs = dsl_.pollMs;
      if (fetchMeta.statusCode <= 0) {
        // Transport-level failures benefit from a stronger cooldown.
        const uint8_t shift = adsbFailureStreak_ > 3 ? 3 : adsbFailureStreak_;
        backoffMs = dsl_.pollMs << shift;
        if (backoffMs > 120000U) {
          backoffMs = 120000U;
        }
      } else if (fetchMeta.statusCode == 429 || fetchMeta.statusCode == 503) {
        backoffMs = dsl_.pollMs * 4U;
        if (backoffMs > 120000U) {
          backoffMs = 120000U;
        }
      }
      adsbBackoffUntilMs_ = nowMs + backoffMs;
      if (dsl_.debug) {
        Serial.printf("[%s] [%s] ADSB cooldown %lus streak=%u status=%d\n",
                      widgetName().c_str(), logTimestamp().c_str(),
                      static_cast<unsigned long>(backoffMs / 1000UL),
                      static_cast<unsigned>(adsbFailureStreak_), fetchMeta.statusCode);
      }
    }
    if (dsl_.source == "http") {
      httpFailureStreak_ = static_cast<uint8_t>(httpFailureStreak_ < 7 ? httpFailureStreak_ + 1 : 7);
      uint32_t backoffMs = dsl_.pollMs;
      if (fetchMeta.statusCode == -2) {
        // TLS preflight low-largest-block should retry soon after memory churn settles.
        backoffMs = 5000U;
      } else if (fetchMeta.statusCode == -3) {
        // Global transport cooldown already throttles requests; keep widget retry short.
        backoffMs = 4000U;
      } else if (fetchMeta.statusCode <= 0) {
        // Transport errors should retry from a short base independent of poll interval.
        const uint8_t shift = httpFailureStreak_ > 4 ? 4 : httpFailureStreak_;
        backoffMs = 3000U << shift;  // 6s, 12s, 24s, 48s, 96s
        if (backoffMs > 30000U) {
          backoffMs = 30000U;
        }
      } else if (fetchMeta.statusCode == 429 || fetchMeta.statusCode == 503) {
        backoffMs = dsl_.pollMs * 4U;
        if (backoffMs > 120000U) {
          backoffMs = 120000U;
        }
      }
      httpBackoffUntilMs_ = nowMs + backoffMs;
      if (dsl_.debug) {
        Serial.printf("[%s] [%s] HTTP cooldown %lus streak=%u status=%d\n",
                      widgetName().c_str(), logTimestamp().c_str(),
                      static_cast<unsigned long>(backoffMs / 1000UL),
                      static_cast<unsigned>(httpFailureStreak_), fetchMeta.statusCode);
      }
    }

    const String next = "net err";
    const bool changed = (status_ != next);
    status_ = next;
    return changed;
  }

  if (dsl_.source == "adsb_nearest") {
    if (dsl_.debug && adsbFailureStreak_ > 0) {
      Serial.printf("[%s] [%s] ADSB recovered after %u failures\n", widgetName().c_str(),
                    logTimestamp().c_str(), static_cast<unsigned>(adsbFailureStreak_));
    }
    adsbFailureStreak_ = 0;
    adsbBackoffUntilMs_ = 0;
  }
  if (dsl_.source == "http") {
    if (dsl_.debug && httpFailureStreak_ > 0) {
      Serial.printf("[%s] [%s] HTTP recovered after %u failures\n", widgetName().c_str(),
                    logTimestamp().c_str(), static_cast<unsigned>(httpFailureStreak_));
    }
    httpFailureStreak_ = 0;
    httpBackoffUntilMs_ = 0;
  }

  bool changed = false;
  applyFieldsFromDoc(doc, changed);

  if (status_ != "ok") {
    status_ = "ok";
    changed = true;
  }

  return changed;
}
