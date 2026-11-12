#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/semantic_state/function_stack/function_stack_types.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

class FunctionStack {
public:
  void enterFunction(FunctionDecl *functionDecl);
  void leaveFunction();
  FunctionDecl *getCurrentDecl() const;
  FunctionDecl *getOutermostDecl() const;
  bool isInsideFunction() const;
  bool isCheckingFunction(const FunctionDecl *functionDecl) const;
  size_t getFunctionDepth() const;
  std::vector<FunctionDecl *> getFunctions() const;
  void rememberCurrentFunction(FuncSymbol *function);
  FuncSymbol *getCurrentFunction() const;
  void enterGroup(NestedGroup *group);
  void leaveGroup();
  bool isAssignedInGroup(llvm::StringRef name) const;
  void rememberReturnCollector(std::vector<ReturnStmt *> *returns);
  std::vector<ReturnStmt *> *getReturnCollector() const;
  void addCollectedReturn(ReturnStmt *returnStmt);
  std::vector<ReturnStmt *> getCollectedReturns() const;
  void rememberYieldCollector(std::vector<YieldExpr *> *yields);
  std::vector<YieldExpr *> *getYieldCollector() const;
  bool isCollectingYields() const;
  void addCollectedYield(YieldExpr *yield);
  void enterTrial();
  void leaveTrial();
  bool isInsideTrial() const;
  void rememberEarlyFunction(const FuncSymbol *function);
  void forgetEarlyFunction();
  const FuncSymbol *getEarlyFunction() const;
  void enterCallback(const Signature *expected);
  void leaveCallback();
  const Signature *getExpectedCallback() const;
  void enterAwaitLoop();
  void leaveAwaitLoop();
  bool isInsideAwaitLoop() const;
  void enterOptionalChain();
  void leaveOptionalChain();
  void markShortCircuit();
  bool isShortCircuiting() const;
  FunctionSnapshot saveBody();
  void keepGroupFunctions(const NestedGroup *group);
  void forgetOuterFunctions();
  void restoreBody(FunctionSnapshot saved);

private:
  std::vector<FunctionDecl *> functions;
  FuncSymbol *currentFunction = nullptr;
  std::vector<NestedGroup *> groups;
  std::vector<ReturnStmt *> *returns = nullptr;
  std::vector<YieldExpr *> *yields = nullptr;
  bool trial = false;
  const FuncSymbol *early = nullptr;
  std::vector<const Signature *> callbacks;
  int awaitLoopDepth = 0;
  std::vector<bool> shortCircuits{false};
};

}
