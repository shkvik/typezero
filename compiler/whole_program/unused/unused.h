#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class Unused {
public:
  explicit Unused(Checker &checker);

  void findUnused(SourceFile &program);

private:
  Checker &checker_;
};

}
