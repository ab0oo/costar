#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace text_entry {

struct Options {
  std::string title;
  std::string subtitle;
  std::string initial;
  bool maskInput = false;
  size_t maxLen = 63;
};

// Returns true when user accepted input with OK, false when cancelled.
bool prompt(const Options& options, std::string& outValue);

}  // namespace text_entry
