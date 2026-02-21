#pragma once

#include <Arduino.h>

namespace dsl {

using VarResolver = bool (*)(void* ctx, const String& name, float& out);

struct ExprContext {
  VarResolver resolver = nullptr;
  void* ctx = nullptr;
};

bool evalExpression(const String& expr, const ExprContext& ctx, float& out);

}  // namespace dsl
