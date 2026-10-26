#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ErrorLowerer {
public:
  explicit ErrorLowerer(Lowering &lowering);

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

}
