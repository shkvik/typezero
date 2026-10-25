#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The functions nothing calls.
class Unused {
public:
  explicit Unused(Checker &checker);

  // The functions of imported files that the program does not use.
  void findUnused(SourceFile &program);

private:
  Checker &checker_;
};

} // namespace tz
