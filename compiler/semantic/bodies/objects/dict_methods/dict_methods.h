#pragma once

#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
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
  struct DictCall {
    CallExpr *call = nullptr;
    const Type *type = nullptr;
    const Type *key = nullptr;
    const Type *value = nullptr;
    Builtin builtin = Builtin::None;
    llvm::StringRef name;
    bool isMap = false;
  };

  static const Type *iteratedType(const DictCall &dict, DictMethod method);
  static ResultDictMethodCalled toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result);

  ResultDictMethodCalled checkGet(const DictCall &dict);
  ResultDictMethodCalled checkSet(const DictCall &dict);
  ResultDictMethodCalled checkAdd(const DictCall &dict);
  ResultDictMethodCalled checkClear(const DictCall &dict);
  ResultDictMethodCalled checkForEach(const DictCall &dict);
  ResultDictMethodCalled checkKeyTest(const DictCall &dict);
  ResultDictMethodCalled checkIterating(const DictCall &dict, DictMethod method);
  ResultDictMethodCalled checkDictArity(CallExpr *call, unsigned min, unsigned max);
  ResultFromEntriesTyped recordFromEntries(CallExpr *call, const Type *element);
  ResultDictMethodCalled checkSetCombine(const DictCall &dict, const DictMethodInfo &found);
  ResultDictMethodCalled recordMethod(CallExpr *call, MemberExpr *member, const std::string &text);
  ResultGroupByCallTyped groupByKey(CallExpr *call, const Type *element, bool map,
                                    const std::string &fn);

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
