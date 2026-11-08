#include "compiler/pipeline/check_program/declare/declare_pass/declare_pass.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/declare/assigned_fields/assigned_fields.h"
#include "compiler/pipeline/check_program/declare/metadata_writers/metadata_writers.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/declare/names_in_functions/names_in_functions.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/topics/child_process/child_checker/child_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_declarer/fs_declarer.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

DeclarePass::DeclarePass(CheckState &owner, SourceFile &program)
    : state(owner),
      file(program) {}

Errors DeclarePass::run() const {
  if (this->file.modules.empty()) {
    Module *module = this->state.getAst().newModule();
    module->isEntry = true;
    module->file.items = this->file.items;
    module->file.imports = std::move(this->file.imports);
    module->file.exports = std::move(this->file.exports);
    module->loadPath = {module};
    this->file.modules.push_back(module);
  }
  this->state.getScopeStack().openScope();
  for (const Feature *feature : this->state.getFeatures().all()) {
    feature->prepare(this->state, this->file);
  }
  for (const Feature *feature : this->state.getFeatures().all()) {
    feature->declare(this->state);
  }
  Prelude(this->state).declarePrelude();

  AssignedFields(this->state).rememberAssigned(this->file);

  std::vector<FunctionDecl *> declared = ModuleDeclarer(this->state).declareModules(this->file);
  this->state.getReturnInference().rememberDeclaredFunctions(std::move(declared));
  MetadataWriters(this->state).rememberWriters(this->file);
  NamesInFunctions(this->state).rememberNames(this->file);

  return this->state.getDiagnostics().take();
}

}
