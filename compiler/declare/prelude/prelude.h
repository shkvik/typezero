#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The types every program has.
class Prelude {
public:
  explicit Prelude(Sema &sema);

  // Computations over types (sema_typeops.cpp, docs/generics.md section
  // 10). The utilities written as mapped types, made once.
  void declarePrelude();

private:
  Sema &sema_;
};

} // namespace tz
