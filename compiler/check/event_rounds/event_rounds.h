#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// A program whose emitters get their event maps from its emit() calls is checked in rounds (see
// EventInference): each round reads it anew, and only the last one says what is wrong and is
// compiled.
class EventRounds {
public:
  EventInference &inference() { return inference_; }

  // Whether another round is needed, with what this one found; the last one once a round finds
  // nothing more.
  bool again();

private:
  EventInference inference_;
  unsigned round_ = 0;
};

} // namespace tz
