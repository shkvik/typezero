#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ReturnChecker {
public:
  explicit ReturnChecker(SemanticState &owner);

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

  SemanticState &state;
};

}
