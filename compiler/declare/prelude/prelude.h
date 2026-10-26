#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class Prelude {
public:
  explicit Prelude(Checker &checker);

  void declarePrelude();

private:
  Checker &checker_;
};

}
