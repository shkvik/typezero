#pragma once

#include "compiler/semantic/inference/destinations/inference_destinations.h"
#include "compiler/semantic/semantic_state/inference/call_cycle_context/call_cycle_context.h"
#include "compiler/semantic/semantic_state/inference/return_inference_context/return_inference_context.h"

namespace tz {

struct InferenceStorage {
  CallCycleContext cycles;
  ReturnInferenceContext returns;
  InferenceDestinations destinations;
};

}
