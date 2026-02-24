#pragma once

#include <cstdint>

namespace RuntimeSettings {
extern bool use24HourClock;
extern bool useFahrenheit;
extern bool useMiles;
extern uint16_t adsbRadiusNm;

void load();
void save();
}  // namespace RuntimeSettings
