#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;

namespace tz {

Prelude::Prelude(SemanticState &owner)
    : state(owner) {}

void Prelude::declarePrelude() {
  llvm::ArrayRef<Stmt *> items =
      this->state.getLibraryDeclarations().getBuiltinItems(BuiltinText::PRELUDE);
  for (Stmt *item : items) {
    TypeAliasDecl *alias = dyn_cast<TypeAliasDecl>(item);
    if (alias != nullptr) {
      this->state.getAliasStore().rememberBuiltinAlias(alias);
    }
  }
}

const Type *Prelude::builtinType(llvm::StringRef name) {
  TypeAliasDecl *alias = this->state.getAliasStore().findBuiltinAlias(name);
  assert(alias && "the prelude declares the type");

  return TypeDeclarer(this->state).resolveAlias(alias);
}

const Type *Prelude::builtinType(llvm::StringRef name, std::vector<const Type *> args) {
  TypeAliasDecl *alias = this->state.getAliasStore().findBuiltinAlias(name);
  assert(alias && "the prelude declares the type");

  return AliasInstantiator(this->state).instantiateAlias(alias, std::move(args), alias->loc());
}

}
