#pragma once

namespace boot {

struct BaselineState {
  unsigned long bootStartMs = 0;
  unsigned long lastLoopLogMs = 0;
};

void start(BaselineState& state);
void mark(BaselineState& state, const char* stage, bool enabled);
void markLoop(BaselineState& state, bool wifiReady, bool enabled, unsigned long periodMs);
void logSettingsSummary(bool includeAdsbRadius);

}  // namespace boot
