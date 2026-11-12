#pragma once

#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class ArgumentChecker;
class CallbackChecker;

class RuntimeObjectChecker {
public:
  RuntimeObjectChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkRuntimeMember(MemberExpr *member);
  Expr *checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  const Type *checkRuntimeTarget(MemberExpr *member);

private:
  Expr *checkArgsLoosely(CallExpr *call);
  Expr *reportNoMember(CallExpr *call, MemberExpr *member);
  bool hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max);
  Expr *checkReadlineMethod(CallExpr *call, MemberExpr *member);
  Expr *reportReadlineEvents(CallExpr *call, MemberExpr *member);
  Expr *checkReadlineClose(CallExpr *call, MemberExpr *member);
  Expr *checkQuestion(CallExpr *call, MemberExpr *member);
  Expr *checkQuestionCallback(CallExpr *call, MemberExpr *member, Expr *query);
  Expr *checkAsyncIteratorMethod(CallExpr *call, MemberExpr *member);

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
