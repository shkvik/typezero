#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class LoadOrder {
public:
  explicit LoadOrder(Checker &checker);

  void checkLoadOrder(SourceFile &program);

private:
  Checker &checker_;
};

}
