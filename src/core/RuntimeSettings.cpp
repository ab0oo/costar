#include "RuntimeSettings.h"

#include "platform/Prefs.h"

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
  use24HourClock = platform::prefs::getBool(kPrefsNs, kClock24Key, use24HourClock);
  useFahrenheit = platform::prefs::getBool(kPrefsNs, kTempFKey, useFahrenheit);
  useMiles = platform::prefs::getBool(kPrefsNs, kMilesKey, useMiles);
  adsbRadiusNm =
      static_cast<uint16_t>(platform::prefs::getUInt(kPrefsNs, kAdsbRadiusKey, adsbRadiusNm));
}

void save() {
  (void)platform::prefs::putBool(kPrefsNs, kClock24Key, use24HourClock);
  (void)platform::prefs::putBool(kPrefsNs, kTempFKey, useFahrenheit);
  (void)platform::prefs::putBool(kPrefsNs, kMilesKey, useMiles);
  (void)platform::prefs::putUInt(kPrefsNs, kAdsbRadiusKey, adsbRadiusNm);
}
}  // namespace RuntimeSettings
