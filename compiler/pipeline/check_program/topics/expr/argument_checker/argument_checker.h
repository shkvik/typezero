#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ArgumentChecker {
public:
  using ResultArityMatches = Result<CallExpr *, ArgumentCheckerCheckArityError>;

  explicit ArgumentChecker(CheckState &owner);

  Expr *checkAnyNumber(Expr *arg);
  void checkArgsLoosely(CallExpr *call);
  Expr *checkArg(Expr *arg, const Type *to);
  ResultArityMatches checkArity(CallExpr *call, unsigned min, unsigned max);
  Expr *checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs,
                       bool hasRest, llvm::SMLoc loc, const std::vector<bool> *checked = nullptr);

private:
  using ResultSpreadRestFits = Result<const Expr *, ArgumentCheckerSpreadRestError>;
  using ResultTupleRestCount = Result<const Expr *, ArgumentCheckerTupleRestError>;

  static ResultSpreadRestFits spreadRest(const Expr *arg, const Type *to);
  static ResultTupleRestCount tupleRest(const ArrayLitExpr *rest, const Type *listRest,
                                        size_t fixed, size_t count);

  CheckState &state;
};

}
