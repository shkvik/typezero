#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ChildErrorTypes {
public:
  explicit ChildErrorTypes(Checker &checker);

  void declare();
  const Type *errorOf(llvm::StringRef fn, bool text, bool shell);
  const Type *spawnResult(bool text);

private:
  Checker &checker_;
};

}
