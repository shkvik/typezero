#pragma once

#include "compiler/pipeline/check_program/overflow/overflow_check/overflow_check.h"

namespace tz {

constexpr unsigned kMaxRounds = 30;

class OverflowHelpers {
public:
  static bool containsLoop(const Stmt *stmt);
  static const Expr *stripCasts(const Expr *expr);
  static std::optional<size_t> callbackArg(Builtin builtin);
  static Val widenVal(const Val &old, const Val &grown, const Type *type);
};

}
