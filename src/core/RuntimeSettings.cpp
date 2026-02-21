#include "RuntimeSettings.h"

#include <Preferences.h>

namespace {
constexpr char kPrefsNs[] = "settings";
constexpr char kClock24Key[] = "clock24";
constexpr char kTempFKey[] = "temp_f";
constexpr char kMilesKey[] = "miles";
constexpr char kAdsbRadiusKey[] = "adsb_radius";
}  // namespace

namespace RuntimeSettings {
bool use24HourClock = false;
bool useFahrenheit = true;
bool useMiles = true;
uint16_t adsbRadiusNm = 40;

void load() {
  Preferences prefs;
  prefs.begin(kPrefsNs, true);
  use24HourClock = prefs.getBool(kClock24Key, use24HourClock);
  useFahrenheit = prefs.getBool(kTempFKey, useFahrenheit);
  useMiles = prefs.getBool(kMilesKey, useMiles);
  adsbRadiusNm = static_cast<uint16_t>(prefs.getUInt(kAdsbRadiusKey, adsbRadiusNm));
  prefs.end();
}

void save() {
  Preferences prefs;
  prefs.begin(kPrefsNs, false);
  prefs.putBool(kClock24Key, use24HourClock);
  prefs.putBool(kTempFKey, useFahrenheit);
  prefs.putBool(kMilesKey, useMiles);
  prefs.putUInt(kAdsbRadiusKey, adsbRadiusNm);
  prefs.end();
}
}  // namespace RuntimeSettings
