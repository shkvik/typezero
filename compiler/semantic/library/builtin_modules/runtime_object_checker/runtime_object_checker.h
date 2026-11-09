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
  const Type *checkRuntimeTarget(MemberExpr *member, bool compound);

private:
  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
