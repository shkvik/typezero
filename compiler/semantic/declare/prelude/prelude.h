#pragma once

#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeDeclarer;
class AliasInstantiator;

class Prelude {
public:
  Prelude(SemanticState &owner, SemanticParts &all);

  void declarePrelude();
  const Type *builtinType(PreludeType type);
  const Type *builtinType(llvm::StringRef name, std::vector<const Type *> args);

private:
  SemanticState &state;
  TypeDeclarer *typeDeclarer = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
};

}
