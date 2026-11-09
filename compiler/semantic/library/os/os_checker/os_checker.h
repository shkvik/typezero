#pragma once

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

  Expr *checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkOsValue(llvm::StringRef name, Expr *expr);

private:
  using ResultOsFunction = Result<llvm::StringRef, OsCheckerOsFunctionError>;

  static ResultOsFunction osFunction(llvm::StringRef fn);
  static const Type *cpuInfoType();
  static const Type *userInfoType();

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
