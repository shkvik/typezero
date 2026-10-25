#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The cycles of calls among functions without a return type, whose types are inferred
// together (Tarjan).
class CallCycles {
public:
  explicit CallCycles(Sema &sema);

  // Finds those cycles among the functions without a return type. A name in
  // a body may call the function of that name among them, or the one
  // `resolve` gives for the function the body is in.
  void
  findCycles(llvm::ArrayRef<FunctionDecl *> functions,
             llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)> resolve = nullptr);

private:
  Sema &sema_;
};

} // namespace tz
