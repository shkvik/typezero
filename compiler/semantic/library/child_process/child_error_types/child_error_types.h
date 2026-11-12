#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class BuiltinModuleTypes;

class ChildErrorTypes {
public:
  ChildErrorTypes(SemanticState &owner, SemanticParts &all);

  const Type *errorOf(llvm::StringRef functionName, bool text, bool shell);
  const Type *spawnResult(bool text);

private:
  SemanticState &state;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
};

}
