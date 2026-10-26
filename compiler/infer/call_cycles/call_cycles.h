#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class CallCycles {
public:
  explicit CallCycles(Checker &checker);

  void
  findCycles(llvm::ArrayRef<FunctionDecl *> functions,
             llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)> resolve = nullptr);

private:
  Checker &checker_;
};

}
