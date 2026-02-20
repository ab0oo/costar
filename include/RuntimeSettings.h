#pragma once

#include <Arduino.h>

namespace RuntimeSettings {
extern bool use24HourClock;
extern bool useFahrenheit;
extern bool useMiles;

void load();
void save();
}  // namespace RuntimeSettings
