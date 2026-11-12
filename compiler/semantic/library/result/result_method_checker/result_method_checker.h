#pragma once

#include "compiler/semantic/library/result/result_method_checker/result_method_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ResultTypes;
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
  const Type *halfParam(const Type *half) const;
  bool isGivingType(const Type *half) const;

  struct ResultCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    const Type *type = nullptr;
    const Type *value = nullptr;
    const Type *error = nullptr;
    Builtin builtin = Builtin::None;
    llvm::StringRef name;
  };

  ResultCall checkResultCall(CallExpr *call, MemberExpr *member) const;
  void walkArgsPlainly(CallExpr *call);
  const Type *wantedType(const Type *expected) const;
  Expr *checkUnwrapOr(const ResultCall &run);
  const Type *halfCallbackReturn(const ResultCall &run, Expr *&arg, const Type *half,
                                 const Type *returnType);
  Expr *checkResultMatch(const ResultCall &run, const Type *expected);
  const Type *callbackReturn(const ResultCall &run, const Type *expected);
  void typeResultCall(const ResultCall &run, const Type *returnType);
  bool reportNotResult(const ResultCall &run, const Type *returnType);
  void typeChainedCall(const ResultCall &run, const Type *returnType);
  void typeOrElseCall(const ResultCall &run, const Type *returnType);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ResultTypes *resultTypes = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
