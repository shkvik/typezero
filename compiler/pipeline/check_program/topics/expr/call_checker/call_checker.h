#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class CallChecker {
public:
  using ResultValueCallTyped = Result<Expr *, CallCheckerCheckValueCallError>;

  explicit CallChecker(CheckState &owner);

  bool rejectSpread(CallExpr *call);
  Expr *checkCall(CallExpr *call, const Type *expected);
  bool isGlobalName(const Expr *expr, llvm::StringRef name) const;
  ResultValueCallTyped checkValueCall(CallExpr *call, const Type *fnType);
  Expr *checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc,
                        const Type *expected = nullptr);

private:
  using ResultNoSpreadPassed = Result<const CallExpr *, CallCheckerSpreadFreeError>;
  using ResultCalleeCallable = Result<const Type *, CallCheckerCalleeCallableError>;
  using ResultTypeArgsAbsent = Result<const MemberExpr *, CallCheckerNoTypeArgsError>;
  using ResultTypeArgsUsable = Result<const FuncSymbol *, CallCheckerDirectTypeArgsError>;

  static ResultTypeArgsAbsent noTypeArgs(const MemberExpr *member);
  static ResultNoSpreadPassed spreadFree(const CallExpr *call, size_t i);
  static ResultCalleeCallable calleeCallable(const Type *type, const NameExpr *calleeName);
  static ResultTypeArgsUsable directTypeArgs(const FuncSymbol *func,
                                             llvm::ArrayRef<TypeRef> typeArgs);

  CheckState &state;
};

}
