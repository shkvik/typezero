#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/semantic_state/function_stack/function_stack_types.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <vector>

namespace tz {

class FunctionStackContext {
public:
  void enterTrial();
  void leaveGroup();
  void leaveTrial();
  void leaveCallback();
  void leaveFunction();
  void enterAwaitLoop();
  void leaveAwaitLoop();
  void markShortCircuit();
  void enterOptionalChain();
  void leaveOptionalChain();
  void forgetEarlyFunction();
  void forgetOuterFunctions();
  void enterGroup(NestedGroup *group);
  void addCollectedYield(YieldExpr *yield);
  void restoreBody(FunctionSnapshot saved);
  void enterFunction(FunctionDecl *functionDecl);
  void addCollectedReturn(ReturnStmt *returnStmt);
  void keepGroupFunctions(const NestedGroup *group);
  void rememberCurrentFunction(FuncSymbol *function);
  void rememberEarlyFunction(const FuncSymbol *function);
  void enterCallback(const std::optional<Signature> &expected);
  void rememberYieldCollector(std::vector<YieldExpr *> *yields);
  void rememberReturnCollector(std::vector<ReturnStmt *> *returns);


  bool isInsideTrial() const;
  bool isInsideFunction() const;
  bool isInsideAwaitLoop() const;
  bool isShortCircuiting() const;
  bool isCollectingYields() const;
  bool isAssignedInGroup(llvm::StringRef name) const;
  bool isCheckingFunction(const FunctionDecl *functionDecl) const;


  FunctionDecl *getCurrentDecl() const;
  FunctionDecl *getOutermostDecl() const;


  FunctionSnapshot saveBody();
  size_t getFunctionDepth() const;
  FuncSymbol *getCurrentFunction() const;
  const FuncSymbol *getEarlyFunction() const;
  const Signature *getExpectedCallback() const;
  std::vector<FunctionDecl *> getFunctions() const;
  std::vector<YieldExpr *> *getYieldCollector() const;
  std::vector<ReturnStmt *> *getReturnCollector() const;
  std::vector<ReturnStmt *> getCollectedReturns() const;

private:
  bool trial = false;
  int awaitLoopDepth = 0;
  const FuncSymbol *early = nullptr;
  std::vector<NestedGroup *> groups;
  FuncSymbol *currentFunction = nullptr;
  std::vector<FunctionDecl *> functions;
  std::vector<bool> shortCircuits{false};
  std::vector<YieldExpr *> *yields = nullptr;
  std::vector<ReturnStmt *> *returns = nullptr;
  std::vector<std::optional<Signature>> callbacks;
};

}
