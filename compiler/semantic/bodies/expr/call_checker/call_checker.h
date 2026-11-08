#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class CallChecker {
public:
  using ResultValueCallTyped = Result<Expr *, CallCheckerCheckValueCallError>;

  explicit CallChecker(SemanticState &owner);

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

  SemanticState &state;
};

}
