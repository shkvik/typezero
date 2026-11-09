#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeDeclarer;
class AliasInstantiator;

class Prelude {
public:
  Prelude(SemanticState &owner, SemanticParts &all);

  void declarePrelude();
  const Type *builtinType(llvm::StringRef name);
  const Type *builtinType(llvm::StringRef name, std::vector<const Type *> args);

private:
  SemanticState &state;
  TypeDeclarer *typeDeclarer = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
};

}
