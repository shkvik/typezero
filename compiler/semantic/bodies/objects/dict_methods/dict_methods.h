#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/dict_methods/dict_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class DictChecker;
class ExprChecker;
class FlowChecker;
class DictNewChecker;
class ArgumentChecker;
class CallbackChecker;

class DictMethods {
public:
  using ResultGroupByCallTyped = Result<Expr *, DictMethodsCheckGroupByError>;
  using ResultMapFunctionTyped = Result<Expr *, DictMethodsCheckMapStaticError>;
  using ResultDictMethodCalled = Result<Expr *, DictMethodsCheckDictMethodError>;
  using ResultFromEntriesTyped = Result<Expr *, DictMethodsCheckFromEntriesError>;
  using ResultDictObjectCalled = Result<Expr *, DictMethodsCheckDictObjectCallError>;

  DictMethods(SemanticState &owner, SemanticParts &all);

  ResultGroupByCallTyped checkGroupBy(CallExpr *call, bool map);
  ResultMapFunctionTyped checkMapStatic(CallExpr *call, MemberExpr *member);
  ResultDictMethodCalled checkDictMethod(CallExpr *call, MemberExpr *member);
  ResultFromEntriesTyped checkFromEntries(CallExpr *call, const Type *expected);
  ResultDictObjectCalled checkDictObjectCall(CallExpr *call, llvm::StringRef fn);

private:
  SemanticState &state;
  CallChecker *callChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  DictNewChecker *dictNewChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
};

}
