#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The functions nothing calls.
class Unused {
public:
  explicit Unused(Sema &sema);

  // The functions of imported files that the program does not use.
  void findUnused(SourceFile &program);

private:
  Sema &sema_;
};

} // namespace tz
