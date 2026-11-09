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

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
