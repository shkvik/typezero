#pragma once

#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/code/functions/function_rules/function_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ClosureBodies {
public:
  ClosureBodies(SemanticState &owner, FunctionRules &sharedFunctionRules);

  void leaveInitializerClosure();
  void declareOwnName(const DeclareOwnName &task);
  void startClosureBody(const StartClosureBody &task);
  void keepConstantFlow(const KeepConstantFlow &task);
  void afterClosureType(const AfterClosureType &task);
  void afterClosureReturn(const AfterClosureReturn &task);
  void checkFunctionValue(const CheckFunctionValue &task);

private:
  void pushClosureBody(FunctionDecl *functionDecl);
  void startRecursiveReturn(FunctionDecl *functionDecl, FuncSymbol *function);


  bool isThisReady(const Flow &flow) const;
  bool isFixedTerm(const Term &term) const;
  bool isRecursiveClosure(FunctionDecl *functionDecl);
  bool isUnsetField(const Flow &flow, const ClassField &field) const;

  SemanticState &state;
  FunctionRules &functionRules;
};

}
