#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The cycles of calls among functions without a return type, whose types are inferred
// together (Tarjan).
class CallCycles {
public:
  explicit CallCycles(Checker &checker);

  // Finds those cycles among the functions without a return type. A name in
  // a body may call the function of that name among them, or the one
  // `resolve` gives for the function the body is in.
  void
  findCycles(llvm::ArrayRef<FunctionDecl *> functions,
             llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)> resolve = nullptr);

private:
  Checker &checker_;
};

} // namespace tz
