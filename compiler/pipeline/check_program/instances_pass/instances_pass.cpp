#include "compiler/pipeline/check_program/instances_pass/instances_pass.h"
#include "compiler/semantic/semantic_external.h"

namespace tz {

InstancesPass::InstancesPass(SemanticExternalFacade &owner)
    : engine(owner) {}

Errors InstancesPass::run() const {
  return this->engine.walkInstances();
}

}
