#include "compiler/semantic/declaration/prelude/declaration_prelude_registry.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;

namespace tz {

DeclarationPreludeRegistry::DeclarationPreludeRegistry(SemanticState &owner)
    : state(owner) {}

void DeclarationPreludeRegistry::declarePrelude() {
  auto items = this->state.library.declarations.getBuiltinItems(BuiltinText::PRELUDE);
  for (Stmt *item : items) {
    auto *alias = dyn_cast<TypeAliasDecl>(item);
    if (alias != nullptr) {
      this->state.declaration.prelude.rememberBuiltinAlias(alias);
    }
  }
}

}
