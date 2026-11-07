#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stmt/return_checker/return_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ReturnChecker {
public:
  explicit ReturnChecker(CheckState &owner);

  void checkReturn(ReturnStmt *ret);
  static bool isExit(const Stmt *stmt);
  Expr *checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested);
  static bool alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {});

private:
  using ResultVoidReturnsEmpty = Result<const Type *, ReturnCheckerVoidReturnError>;
  using ResultValueNotRequired = Result<const Type *, ReturnCheckerValueRequiredError>;
  using ResultPromiseTypeMatch = Result<const Type *, ReturnCheckerPromiseReturnError>;
  using ResultGeneratorReturns = Result<const Type *, ReturnCheckerGeneratorReturnError>;
  using ResultAwaitNotReturned = Result<const ReturnStmt *, ReturnCheckerAwaitReturnedError>;
  using ResultInsideOfFunction = Result<const FuncSymbol *, ReturnCheckerEnclosingFunctionError>;
  using ResultConstructorExits = Result<const FuncSymbol *, ReturnCheckerConstructorReturnError>;

  static bool isTrueLiteral(const Expr *expr);
  ResultInsideOfFunction enclosingFunction() const;
  static bool breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels);
  static ResultVoidReturnsEmpty voidReturn(const Type *type, const FuncSymbol *fn);
  static ResultValueNotRequired valueRequired(const Type *returned, const FuncSymbol *fn);
  static ResultAwaitNotReturned awaitReturned(const ReturnStmt *ret, const FuncSymbol *fn);
  ResultConstructorExits constructorReturn(const ReturnStmt *ret, const FuncSymbol *fn) const;
  static ResultPromiseTypeMatch promiseReturn(const Type *given, const Type *returned,
                                              const FuncSymbol *fn);
  static ResultGeneratorReturns generatorReturn(const Type *given, const Type *returned,
                                                const FuncSymbol *fn);

  CheckState &state;
};

}
