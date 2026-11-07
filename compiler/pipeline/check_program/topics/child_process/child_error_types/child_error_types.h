#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ChildErrorTypes {
public:
  explicit ChildErrorTypes(CheckState &owner);

  void declare();
  const Type *errorOf(llvm::StringRef fn, bool text, bool shell);
  const Type *spawnResult(bool text);

private:
  CheckState &state;
};

}
