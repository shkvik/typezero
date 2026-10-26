#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ReturnInferrer {
public:
  explicit ReturnInferrer(Checker &checker);

  const Type *returnType(FuncSymbol *fn);
  const Type *functionType(FuncSymbol *fn);
  void inferReturnType(FuncSymbol *fn);
  void resolveParams(FuncSymbol *fn);
  void checkAndInfer(FunctionDecl *decl);
  void inferCycle(llvm::ArrayRef<FuncSymbol *> cycle);
  const Type *unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                           bool fallsOff, bool report);
  FunctionDecl *freshCopy(const FunctionDecl *fn);
  static bool returnsValue(const Stmt *stmt);
  Checker::BodyState enterBody(const FunctionDecl *fn);
  void leaveBody(Checker::BodyState &saved);

private:
  Checker &checker_;
};

}
