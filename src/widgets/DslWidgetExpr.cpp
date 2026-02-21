#include "widgets/DslWidget.h"

#include "dsl/DslExpr.h"

bool DslWidget::getNumeric(const String& key, float& out) const {
  auto it = values_.find(key);
  if (it == values_.end()) {
    return false;
  }

  const String& text = it->second;
  bool hasDigit = false;
  String filtered;
  filtered.reserve(text.length());
  for (int i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+') {
      filtered += c;
      if (c >= '0' && c <= '9') {
        hasDigit = true;
      }
    }
  }

  if (!hasDigit) {
    return false;
  }

  out = filtered.toFloat();
  return true;
}

bool DslWidget::resolveNumericVar(void* ctx, const String& name, float& out) {
  auto* widget = static_cast<DslWidget*>(ctx);
  return widget->getNumeric(name, out);
}

bool DslWidget::evaluateAngleExpr(const String& expr, float& outDegrees) const {
  const String e = bindRuntimeTemplate(expr);
  dsl::ExprContext ctx;
  ctx.resolver = &DslWidget::resolveNumericVar;
  ctx.ctx = const_cast<DslWidget*>(this);
  return dsl::evalExpression(e, ctx, outDegrees);
}
