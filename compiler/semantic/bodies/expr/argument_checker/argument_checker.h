#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class FlowChecker;
class TypeRelations;
class ArgListChecker;

class ArgumentChecker {
public:
  using ResultArityMatches = Result<CallExpr *, ArgumentCheckerCheckArityError>;

  ArgumentChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkAnyNumber(Expr *arg);
  void walkArgsLoosely(CallExpr *call);
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

  struct ArgumentShape {
    llvm::ArrayRef<const Type *> params;
    size_t fixed;
    llvm::SMLoc loc;
    const std::vector<bool> *checked;
  };

  static bool isChecked(const ArgumentShape &shape, size_t i);
  bool rejectsSpreadBefore(CallExpr *call, size_t limit);
  Expr *checkListParamArguments(CallExpr *call, const ArgumentShape &shape, unsigned least);
  Expr *packRestArgs(CallExpr *call, const ArgumentShape &shape);
  Expr *checkSpreadRest(CallExpr *call, const ArgumentShape &shape, const Type *listRest);
  Expr *packRestElements(CallExpr *call, const ArgumentShape &shape, const Type *listRest);
  Expr *checkedRestElement(CallExpr *call, size_t i, const ArgumentShape &shape,
                           const Type *listRest);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgListChecker *argListChecker = nullptr;
};

}
