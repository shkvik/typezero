#include "compiler/semantic/semantic_external.h"
#include "compiler/program/program/program.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

SemanticExternalFacade::SemanticExternalFacade(Program &program)
    : file(program.file),
      state(std::make_unique<SemanticState>(program.ast, program.diags, program.file)),
      parts(std::make_unique<SemanticParts>(*this->state)) {}

SemanticExternalFacade::~SemanticExternalFacade() = default;

SemanticExternalFacade::ResultProgramChecked SemanticExternalFacade::run() {
  auto walked = this->parts->walkProgram();
  std::vector<ClassDecl *> &late = this->file.inventory.lateClasses;
  this->file.items.insert(this->file.items.end(), late.begin(), late.end());
  if (walked.isErr()) {
    ItemList<Diagnostic> found;
    found.append(walked.error());

    return err(found);
  }

  return ok();
}

}
