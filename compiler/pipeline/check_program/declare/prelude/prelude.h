#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class Prelude {
public:
  explicit Prelude(CheckState &owner);

  void declarePrelude();
  const Type *builtinType(llvm::StringRef name);
  const Type *builtinType(llvm::StringRef name, std::vector<const Type *> args);

private:
  CheckState &state;
};

}
