#pragma once

#include "compiler/semantic/semantic_external.h"

namespace tz {

class EventRounds {
public:
  EventInference &inference() { return inference_; }

  bool again();

private:
  EventInference inference_;
  unsigned round = 0;
};

}
