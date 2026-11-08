#include "compiler/pipeline/check_program/check_pass/check_pass.h"

namespace tz {

CheckPass::CheckPass(SemanticExternalFacade &owner)
    : engine(owner) {}

CheckPass::ResultChecked CheckPass::run() const {
  return this->engine.walkBodies();
}

}
