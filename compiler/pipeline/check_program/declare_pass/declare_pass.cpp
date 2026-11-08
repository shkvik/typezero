#include "compiler/pipeline/check_program/declare_pass/declare_pass.h"
#include "compiler/program/program/program.h"
#include "compiler/semantic/semantic_external.h"

namespace tz {

DeclarePass::DeclarePass(SemanticExternalFacade &owner, Program &checked)
    : engine(owner),
      program(checked) {}

Errors DeclarePass::run() const {
  SourceFile &file = this->program.file;
  if (file.modules.empty()) {
    Module *module = this->program.asts.back()->newModule();
    module->isEntry = true;
    module->file.items = file.items;
    module->file.imports = std::move(file.imports);
    module->file.exports = std::move(file.exports);
    module->loadPath = {module};
    file.modules.push_back(module);
  }

  return this->engine.declareProgram();
}

}
