#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/calls/call_rules/call_rules.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <utility>
#include <vector>

namespace tz {

class CallArguments {
public:
  CallArguments(SemanticState &owner, CallRules &sharedCallRules);

  void checkArgSlot(const CheckArgSlot &task);
  void checkValueCall(const CheckValueCall &task);
  void checkArguments(const CheckArguments &task);
  void afterSpreadRest(const AfterSpreadRest &task);
  void walkArgsLoosely(const WalkArgsLoosely &task);
  void afterCalleeValue(const AfterCalleeValue &task);
  void afterDirectReturnType(const AfterDirectReturnType &task);

private:
  struct ArgumentShape {
    llvm::ArrayRef<const Type *> params;
    size_t fixed;
    llvm::SMLoc loc;
    std::vector<bool> checked;
  };

  using ResultTupleRestCount = Result<const Expr *, CodeCallsRegistryTupleRestError>;
  using ResultSpreadRestFits = Result<const Expr *, CodeCallsRegistrySpreadRestError>;
  using ResultArityMatches = Result<const CallExpr *, CodeCallsRegistryCheckArityError>;
  using ResultCalleeCallable = Result<const Type *, CodeCallsRegistryCalleeCallableError>;

  void startRestArguments(CallExpr *call, const ArgumentShape &shape);
  void replaceRestArgs(CallExpr *call, size_t fixed, ArrayLitExpr *rest);
  void pushTypedArguments(CallExpr *call, const ArgumentShape &shape, size_t count);
  void startListParamArguments(CallExpr *call, const ArgumentShape &shape, unsigned least);


  bool rejectsSpreadBefore(CallExpr *call, size_t limit);
  bool isChecked(const ArgumentShape &shape, size_t index) const;
  bool packRestElements(CallExpr *call, const ArgumentShape &shape, const Type *listRest,
                        std::vector<std::pair<Expr **, const Type *>> &coercions);


  ResultSpreadRestFits checkRestType(const Expr *arg, const Type *to) const;
  ResultCalleeCallable checkCalleeCallable(const Type *type, const NameExpr *calleeName) const;
  ResultTupleRestCount checkTupleRestCount(const ArrayLitExpr *rest, const Type *listRest,
                                           size_t fixed, size_t count) const;


  ResultArityMatches checkArity(const CallExpr *call, unsigned min, unsigned max) const;
  const Type *restElementType(size_t index, const ArgumentShape &shape, const Type *listRest) const;

  CallRules &callRules;
  SemanticState &state;
};

}
