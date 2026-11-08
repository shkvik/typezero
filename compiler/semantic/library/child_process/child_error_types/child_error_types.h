#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ChildErrorTypes {
public:
  explicit ChildErrorTypes(SemanticState &owner);

  void declare();
  const Type *errorOf(llvm::StringRef fn, bool text, bool shell);
  const Type *spawnResult(bool text);

private:
  SemanticState &state;
};

}
