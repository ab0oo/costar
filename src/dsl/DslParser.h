#pragma once

#include <Arduino.h>

#include "dsl/DslModel.h"

namespace dsl {

class Parser {
 public:
  static bool parseFile(const String& path, Document& out, String* error = nullptr);
};

}  // namespace dsl
