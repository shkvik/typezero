#include "compiler/semantic/declare/names_in_functions/names_in_functions.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/StringSet.h"

using llvm::isa;

namespace tz {

NamesInFunctions::NamesInFunctions(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->importBinder = &all.importBinder;
}

void NamesInFunctions::rememberNames(SourceFile &program) {
  auto names = llvm::StringSet<>();
  for (Stmt *item : program.items) {
    AstWalker::collectNames(item, names, isa<FunctionDecl>(item));
  }
  for (const auto &name : names) {
    this->state.getScopeStack().markUsedInFunction(name.getKey());
  }
  this->state.getScopeStack().markUsedInFunction(LibraryModuleNames::kSubtleName);
  this->importBinder->noteImportsInFunctions(program);
}

}
