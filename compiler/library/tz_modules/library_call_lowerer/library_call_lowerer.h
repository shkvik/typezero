#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The intrinsics __tz_* of the built-in modules written in tz, and AbortSignal.
class LibraryCallLowerer {
public:
  explicit LibraryCallLowerer(CodeGen &codegen);

  // The intrinsics of the built-in modules written in tz, `__tz_*`
  // (codegen_events.cpp).
  llvm::Value *emitLibBuiltin(CallExpr *call);
  // The runtime's side of the AbortSignal `signal` of `type` (an AbortSignal
  // of library/events/web.tz, or undefined): its field #native, borrowed or
  // retained, or null.
  llvm::Value *nativeSignal(llvm::Value *signal, const Type *type, bool retain = false);
  static bool isAbortSignal(const Type *type);

private:
  CodeGen &codegen_;
};

} // namespace tz
