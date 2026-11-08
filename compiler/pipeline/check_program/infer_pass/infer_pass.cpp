#include "compiler/pipeline/check_program/infer_pass/infer_pass.h"
#include "compiler/semantic/semantic_external.h"

namespace tz {

InferPass::InferPass(SemanticExternalFacade &owner)
    : engine(owner) {}

Errors InferPass::run() const {
  return this->engine.inferProgram();
}

}
