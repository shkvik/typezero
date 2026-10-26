#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class EventRounds {
public:
  EventInference &inference() { return inference_; }

  bool again();

private:
  EventInference inference_;
  unsigned round_ = 0;
};

}
