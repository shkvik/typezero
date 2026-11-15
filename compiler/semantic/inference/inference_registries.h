#pragma once

#include "compiler/semantic/inference/cycles/inference_cycles_registry.h"
#include "compiler/semantic/inference/returns/inference_returns_registry.h"

namespace tz {

struct InferenceRegistries {
  InferenceCyclesRegistry cycles;
  InferenceReturnsRegistry returns;
};

}
