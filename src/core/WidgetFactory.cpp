#include "core/WidgetFactory.h"

#include "widgets/DslWidget.h"

std::unique_ptr<Widget> WidgetFactory::create(const WidgetConfig& cfg) {
  if (cfg.type == "dsl") {
    return std::make_unique<DslWidget>(cfg);
  }
  return nullptr;
}
