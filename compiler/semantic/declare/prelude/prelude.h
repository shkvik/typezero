#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class Prelude {
public:
  explicit Prelude(SemanticState &owner);

  void declarePrelude();
  const Type *builtinType(llvm::StringRef name);
  const Type *builtinType(llvm::StringRef name, std::vector<const Type *> args);

private:
  SemanticState &state;
};

}
