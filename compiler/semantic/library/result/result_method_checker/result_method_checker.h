#pragma once

#include "compiler/semantic/library/result/result_method_checker/result_method_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class FlowChecker;
class TypeRelations;
class ArgumentChecker;
class CallbackChecker;

class ResultMethodChecker {
public:
  ResultMethodChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static const Type *halfParam(const Type *half);
  static bool givesType(const Type *half);

  struct ResultCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    const Type *type = nullptr;
    const Type *value = nullptr;
    const Type *error = nullptr;
    Builtin builtin = Builtin::None;
    llvm::StringRef name;
  };

  static ResultCall resultCall(CallExpr *call, MemberExpr *member);
  void walkArgsPlainly(CallExpr *call);
  static const Type *wantedType(const Type *expected);
  Expr *checkUnwrapOr(const ResultCall &run);
  const Type *halfCallbackReturn(const ResultCall &run, Expr *&arg, const Type *half,
                                 const Type *ret);
  Expr *checkResultMatch(const ResultCall &run, const Type *expected);
  const Type *callbackReturn(const ResultCall &run, const Type *expected);
  void typeResultCall(const ResultCall &run, const Type *ret);
  bool returnsResult(const ResultCall &run, const Type *ret);
  void typeChainedCall(const ResultCall &run, const Type *ret);
  void typeOrElseCall(const ResultCall &run, const Type *ret);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
