#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/dict_methods/dict_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class DictMethods {
public:
  using ResultGroupByCallTyped = Result<Expr *, DictMethodsCheckGroupByError>;
  using ResultMapFunctionTyped = Result<Expr *, DictMethodsCheckMapStaticError>;
  using ResultDictMethodCalled = Result<Expr *, DictMethodsCheckDictMethodError>;
  using ResultFromEntriesTyped = Result<Expr *, DictMethodsCheckFromEntriesError>;
  using ResultDictObjectCalled = Result<Expr *, DictMethodsCheckDictObjectCallError>;

  explicit DictMethods(SemanticState &owner);

  ResultGroupByCallTyped checkGroupBy(CallExpr *call, bool map);
  ResultMapFunctionTyped checkMapStatic(CallExpr *call, MemberExpr *member);
  ResultDictMethodCalled checkDictMethod(CallExpr *call, MemberExpr *member);
  ResultFromEntriesTyped checkFromEntries(CallExpr *call, const Type *expected);
  ResultDictObjectCalled checkDictObjectCall(CallExpr *call, llvm::StringRef fn);

private:
  SemanticState &state;
};

}
