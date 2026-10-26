#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class Prelude {
public:
  explicit Prelude(Checker &checker);

  void declarePrelude();
  const Type *builtinType(llvm::StringRef name);
  const Type *builtinType(llvm::StringRef name, std::vector<const Type *> args);

private:
  Checker &checker_;
};

}
