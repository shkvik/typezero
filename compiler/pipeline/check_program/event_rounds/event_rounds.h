#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

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
