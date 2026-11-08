#include "compiler/semantic/declare/names_in_functions/names_in_functions.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/StringSet.h"

using llvm::isa;

namespace tz {

NamesInFunctions::NamesInFunctions(SemanticState &owner)
    : state(owner) {}

void NamesInFunctions::rememberNames(SourceFile &program) {
  llvm::StringSet<> names;
  for (Stmt *item : program.items) {
    AstWalker::collectNames(item, names, isa<FunctionDecl>(item));
  }
  for (const auto &name : names) {
    this->state.getScopeStack().markUsedInFunction(name.getKey());
  }
  this->state.getScopeStack().markUsedInFunction(LibraryModuleNames::kSubtleName);
  ImportBinder(this->state).noteImportsInFunctions(program);
}

}
