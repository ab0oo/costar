#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <map>
#include <vector>

#include <TFT_eSPI.h>

#include "core/Widget.h"
#include "dsl/DslModel.h"
#include "services/HttpJsonClient.h"

class DslWidget final : public Widget {
 public:
  explicit DslWidget(const WidgetConfig& cfg);
  ~DslWidget() override;

  void begin() override;
  bool isNetworkWidget() const override {
    return dslLoaded_ && (dsl_.source == "http" || dsl_.source == "adsb_nearest");
  }
  bool update(uint32_t nowMs) override;
  void render(TFT_eSPI& tft) override;

 private:
  bool loadDslModel();
  String bindTemplate(const String& input) const;
  String bindRuntimeTemplate(const String& input) const;
  bool buildLocalTimeDoc(JsonDocument& outDoc, String& error) const;
  bool buildAdsbNearestDoc(const JsonDocument& rawDoc, JsonDocument& outDoc, String& error) const;
  float distanceKm(float lat1, float lon1, float lat2, float lon2) const;
  bool applyFieldsFromDoc(const JsonDocument& doc, bool& changed);
  bool computeMoonPhaseName(String& out) const;
  bool computeMoonPhaseFraction(float& out) const;
  uint32_t computeAdsbJitterMs(uint32_t pollMs) const;
  bool getNumeric(const String& key, float& out) const;
  static bool resolveNumericVar(void* ctx, const String& name, float& out);
  bool evaluateAngleExpr(const String& expr, float& outDegrees) const;

  bool resolveVariant(const JsonDocument& doc, const String& path,
                      JsonVariantConst& out) const;
  String toText(JsonVariantConst value) const;

  String applyFormat(const String& text, const dsl::FormatSpec& fmt,
                     bool numeric, double numericValue) const;
  String formatNumericLocale(double value, int decimals, const String& locale) const;
  bool parseTzOffsetMinutes(const String& tz, int& minutes) const;
  bool parseIsoMinuteTimestamp(const String& text, int& year, int& mon, int& day, int& hour,
                               int& minute) const;
  long long daysFromCivil(int year, int mon, int day) const;
  void civilFromDays(long long z, int& year, int& mon, int& day) const;
  String formatTimestampWithTz(const String& text, const String& tz,
                               const String& timeFormat) const;

  String dslPath_;
  bool debugOverride_ = false;
  bool useSprite_ = false;
  bool spriteReady_ = false;
  TFT_eSprite* sprite_ = nullptr;
  dsl::Document dsl_;
  bool dslLoaded_ = false;
  String status_ = "init";
  uint32_t lastFetchMs_ = 0;
  uint32_t nextFetchMs_ = 0;
  bool firstFetch_ = true;
  uint32_t adsbBackoffUntilMs_ = 0;
  uint8_t adsbFailureStreak_ = 0;

  std::map<String, String> values_;
  std::map<String, std::vector<float>> seriesValues_;

  HttpJsonClient http_;
};
