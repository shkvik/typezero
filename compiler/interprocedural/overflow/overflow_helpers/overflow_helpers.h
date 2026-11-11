#pragma once

#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"

namespace tz {

constexpr unsigned kMaxRounds = 30;

class OverflowHelpers {
public:
  static bool containsLoop(const Stmt *stmt);
  static const Expr *stripCasts(const Expr *expr);
  static std::optional<size_t> callbackArg(Builtin builtin);
  static Val widenVal(const Val &old, const Val &grown, const Type *type);

private:
  struct WidenLink {
    const Val *old = nullptr;
    const Val *grown = nullptr;
    const Type *type = nullptr;
  };

  static Val widenLink(const WidenLink &link);
  static Num widenNum(const Num &old, Num grown, Limits limits);
  static bool isLoopHere(const Stmt *stmt);
  static std::vector<const Stmt *> loopParts(const Stmt *stmt);
  static bool passesCallback(const Stmt *stmt);
};

}
