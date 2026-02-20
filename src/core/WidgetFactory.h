#pragma once

#include <memory>

#include "Widget.h"

class WidgetFactory {
 public:
  static std::unique_ptr<Widget> create(const WidgetConfig& cfg);
};
