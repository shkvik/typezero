#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/infer/return_inferrer/return_inferrer_errors.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

class ReturnInferrer {
public:
  explicit ReturnInferrer(Checker &owner);

  static bool returnsValue(const Stmt *stmt);

  void resolveParams(FuncSymbol *fn);
  void inferReturnType(FuncSymbol *fn);
  void checkAndInfer(FunctionDecl *decl);
  const Type *returnType(FuncSymbol *fn);
  const Type *functionType(FuncSymbol *fn);
  void leaveBody(Checker::BodyState &saved);
  FunctionDecl *freshCopy(const FunctionDecl *fn);
  void inferCycle(llvm::ArrayRef<FuncSymbol *> cycle);
  Checker::BodyState enterBody(const FunctionDecl *fn);

private:
  using ResultCycleTypeKnown = Result<const Type *, ReturnInferrerCycleTypeError>;
  using ResultParamTypeKnown = Result<const Type *, ReturnInferrerDefaultTypeError>;
  using ResultReturnsUnified = Result<const Type *, ReturnInferrerCommonReturnError>;
  using ResultNotCalledEarly = Result<const FuncSymbol *, ReturnInferrerEarlyCallError>;
  using ResultReturnsDefined = Result<const FuncSymbol *, ReturnInferrerDefinedReturnError>;

  static std::vector<ReturnStmt *> literalsLast(llvm::ArrayRef<ReturnStmt *> returns);

  const Type *trialReturn(FuncSymbol *fn);
  const Type *templateReturn(FuncSymbol *fn);
  ResultNotCalledEarly earlyCall(const FuncSymbol *fn) const;
  const Type *instanceParam(FuncSymbol *fn, const Type *type);
  ResultParamTypeKnown defaultType(const Param &param, const Type *found) const;
  ResultCycleTypeKnown cycleType(const FuncSymbol *fn, int cycleSize, bool failed) const;
  ResultReturnsUnified commonReturn(const FuncSymbol *fn, const Type *before,
                                    const ReturnStmt *ret) const;
  const Type *unifyReturns(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                           bool fallsOff, bool report);
  ResultReturnsDefined definedReturn(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                                     bool undefined) const;
  void convertReturns(const FunctionDecl *decl, llvm::ArrayRef<ReturnStmt *> returns,
                      llvm::ArrayRef<YieldExpr *> yields);

  Checker &checker;
};

}
