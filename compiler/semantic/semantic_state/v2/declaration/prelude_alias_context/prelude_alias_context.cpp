#include "compiler/semantic/semantic_state/v2/declaration/prelude_alias_context/prelude_alias_context.h"

namespace tz {

void PreludeAliasContext::rememberBuiltinAlias(TypeAliasDecl *alias) {
  this->builtinAliases[alias->name] = alias;
}

TypeAliasDecl *PreludeAliasContext::findBuiltinAlias(llvm::StringRef name) const {
  return this->builtinAliases.lookup(name);
}

}
