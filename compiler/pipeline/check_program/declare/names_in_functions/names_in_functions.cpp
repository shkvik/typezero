#include "compiler/pipeline/check_program/declare/names_in_functions/names_in_functions.h"
#include "compiler/load/library_modules/library_modules.h"
#include "compiler/pipeline/check_program/ast_walker/ast_walker.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/declare/import_binder/import_binder.h"
#include "llvm/ADT/StringSet.h"

using llvm::isa;

namespace tz {

NamesInFunctions::NamesInFunctions(CheckState &owner)
    : state(owner) {}

void NamesInFunctions::rememberNames(SourceFile &program) {
  llvm::StringSet<> names;
  for (Stmt *item : program.items) {
    AstWalker::collectNames(item, names, isa<FunctionDecl>(item));
  }
  for (const auto &name : names) {
    this->state.getScopeStack().markUsedInFunction(name.getKey());
  }
  this->state.getScopeStack().markUsedInFunction(kSubtleName);
  ImportBinder(this->state).noteImportsInFunctions(program);
}

}
