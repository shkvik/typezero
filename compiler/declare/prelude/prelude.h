#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The types every program has.
class Prelude {
public:
  explicit Prelude(Checker &checker);

  // Computations over types (sema_typeops.cpp, docs/generics.md section
  // 10). The utilities written as mapped types, made once.
  void declarePrelude();

private:
  Checker &checker_;
};

} // namespace tz
