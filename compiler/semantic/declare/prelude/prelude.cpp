#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;

namespace tz {

Prelude::Prelude(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeDeclarer = &all.typeDeclarer;
  this->aliasInstantiator = &all.aliasInstantiator;
}

void Prelude::declarePrelude() {
  auto items = this->state.getLibraryDeclarations().getBuiltinItems(BuiltinText::PRELUDE);
  for (Stmt *item : items) {
    auto *alias = dyn_cast<TypeAliasDecl>(item);
    if (alias != nullptr) {
      this->state.getAliasStore().rememberBuiltinAlias(alias);
    }
  }
}

const Type *Prelude::builtinType(PreludeType type) {
  auto name = PreludeTypeTable::getName(type);
  auto *alias = this->state.getAliasStore().findBuiltinAlias(name);
  assert(alias != nullptr && "the prelude declares the type");

  return this->typeDeclarer->resolveAlias(alias);
}

const Type *Prelude::builtinType(llvm::StringRef name, std::vector<const Type *> args) {
  auto *alias = this->state.getAliasStore().findBuiltinAlias(name);
  assert(alias != nullptr && "the prelude declares the type");

  return this->aliasInstantiator->instantiateAlias(alias, std::move(args), alias->loc());
}

}
