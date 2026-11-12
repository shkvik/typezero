#pragma once

#include "compiler/program/library/os/os_function_table/os_function_table.h"
#include "compiler/program/library/os/os_value_table/os_value_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/os/os_checker/os_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class ArgumentChecker;

class OsChecker {
public:
  OsChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkOsCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  Expr *checkOsValue(llvm::StringRef name, Expr *expr);

private:
  using ResultOsFunction = Result<llvm::StringRef, OsCheckerOsFunctionError>;

  ResultOsFunction checkKnownOsFunction(llvm::StringRef functionName) const;
  const Type *cpuInfoType() const;
  llvm::StringRef osValueText(OsValue value) const;
  const Type *userInfoType() const;
  bool hasRejectedArgs(CallExpr *call, llvm::SMLoc loc);
  Expr *checkOsFunction(CallExpr *call, OsFunction function, llvm::SMLoc loc);
  Expr *osStringCall(CallExpr *call, llvm::SMLoc loc, int which);
  Expr *osNumberCall(CallExpr *call, llvm::SMLoc loc, int which);
  void setWhichArg(CallExpr *call, llvm::SMLoc loc, int which);
  Expr *builtinOsCall(CallExpr *call, Builtin builtin, const Type *result) const;
  Expr *endiannessLiteral(llvm::SMLoc loc);
  Expr *reportNotYet(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
