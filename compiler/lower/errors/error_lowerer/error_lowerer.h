#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Error and its kinds: their classes and new.
class ErrorLowerer {
public:
  explicit ErrorLowerer(Lowering &lowering);

  // The class Error (ClassDecl::builtin): its constructor, its descriptor
  // for each kind, the function the runtime makes errors with, and what main
  // tells the runtime about it.
  void emitErrorConstructor(FunctionDecl *fn);
  const ClassDecl *errorKindClass(int kind) const;
  llvm::Constant *errorDescriptor(int kind);
  llvm::Function *errorMaker();
  void emitErrorSupport();
  llvm::Value *emitNewError(NewExpr *expr);
  llvm::Value *emitErrorBuiltin(CallExpr *call);

private:
  Lowering &lowering_;
  llvm::Function *errorMaker_ = nullptr;
};

} // namespace tz
