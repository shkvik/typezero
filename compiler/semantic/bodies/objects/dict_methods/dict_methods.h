#pragma once

#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
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

enum class GroupByOwner {
  MAP,
  OBJECT,
};

class DictMethods {
public:
  using ResultGroupByCallTyped = Result<Expr *, DictMethodsCheckGroupByError>;
  using ResultMapFunctionTyped = Result<Expr *, DictMethodsCheckMapStaticError>;
  using ResultDictMethodCalled = Result<Expr *, DictMethodsCheckDictMethodError>;
  using ResultFromEntriesTyped = Result<Expr *, DictMethodsCheckFromEntriesError>;
  using ResultDictObjectCalled = Result<Expr *, DictMethodsCheckDictObjectCallError>;

  DictMethods(SemanticState &owner, SemanticParts &all);

  ResultGroupByCallTyped checkGroupBy(CallExpr *call, GroupByOwner owner);
  ResultMapFunctionTyped checkMapStatic(CallExpr *call, MemberExpr *member);
  ResultDictMethodCalled checkDictMethod(CallExpr *call, MemberExpr *member);
  ResultFromEntriesTyped checkFromEntries(CallExpr *call, const Type *expected);
  ResultDictObjectCalled checkDictObjectCall(CallExpr *call, llvm::StringRef functionName);
  Builtin objectCallBuiltin(std::optional<GlobalMember> member) const;
  const Type *objectCallType(std::optional<GlobalMember> member, const Type *value) const;

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

  ResultDictMethodCalled checkRecordMethod(CallExpr *call, MemberExpr *member,
                                           const std::string &text);
  ResultDictMethodCalled checkDictArity(CallExpr *call, unsigned min, unsigned max);
  ResultDictMethodCalled toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) const;
  ResultDictMethodCalled checkGet(const DictCall &dict);
  ResultDictMethodCalled checkSet(const DictCall &dict);
  ResultDictMethodCalled checkAdd(const DictCall &dict);
  ResultDictMethodCalled checkKeyTest(const DictCall &dict);
  ResultDictMethodCalled checkClear(const DictCall &dict);
  ResultDictMethodCalled checkIterating(const DictCall &dict, DictMethod method);
  const Type *iteratedType(const DictCall &dict, DictMethod method) const;
  ResultDictMethodCalled checkForEach(const DictCall &dict);
  ResultDictMethodCalled checkSetCombine(const DictCall &dict, const DictMethodInfo &found);
  const Type *walkEntriesSource(CallExpr *call, const Type *record);
  ResultFromEntriesTyped checkEntriesRecord(CallExpr *call, const Type *element);
  ResultGroupByCallTyped checkGroupKey(CallExpr *call, const Type *element, GroupByOwner owner,
                                       const std::string &functionName);
  ResultMapFunctionTyped checkMapGroupBy(CallExpr *call);
  DictMethodsCheckMapStaticErrorCode mapStaticCode(DictMethodsCheckGroupByErrorCode code) const;

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
