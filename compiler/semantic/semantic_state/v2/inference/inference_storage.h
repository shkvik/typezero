#pragma once

#include "compiler/semantic/inference_v2/destinations/inference_destinations.h"
#include "compiler/semantic/semantic_state/v2/inference/call_cycle_context/call_cycle_context.h"
#include "compiler/semantic/semantic_state/v2/inference/return_inference_context/return_inference_context.h"

namespace tz {

struct InferenceStorage {
  CallCycleContext cycles;
  ReturnInferenceContext returns;
  InferenceDestinations destinations;
};

}
