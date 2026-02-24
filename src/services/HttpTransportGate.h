#pragma once

#include <Arduino.h>

namespace httpgate {

class Guard {
 public:
  explicit Guard(uint32_t timeoutMs);
  ~Guard();

  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
};

}  // namespace httpgate

