#include "compiler/pipeline/check_program/check_pass/check_pass.h"
#include "compiler/semantic/semantic_external.h"

namespace tz {

CheckPass::CheckPass(SemanticExternalFacade &owner)
    : engine(owner) {}

CheckPass::ResultChecked CheckPass::run() const {
  Errors taken = this->engine.walkBodies();
  if (taken) {
    return err(taken);
  }
  if (taken.isEmpty()) {
    PassDone done = NoWarnings{};

    return ok(done);
  }

  Warnings warnings;
  for (const Diagnostic &diagnostic : taken) {
    warnings.add(diagnostic);
  }
  PassDone done = WithWarnings{.warnings = warnings};

  return ok(done);
}

}
