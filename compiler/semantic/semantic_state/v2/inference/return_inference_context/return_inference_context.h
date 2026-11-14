#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/semantic_state/v2/checkpoint/checkpoint.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <deque>
#include <vector>

namespace tz {

struct CollectedReturns {
  std::vector<ReturnStmt *> returns;
  std::vector<YieldExpr *> yields;
};

class ReturnInferenceContext {
public:
  void leaveInference();
  void enterModuleLoad();
  void leaveModuleLoad();
  void pushSavedBody(Checkpoint saved);
  void enterInference(const FuncSymbol *function);
  void forgetSuspended(const FuncSymbol *function);
  void rememberInheritedReturn(const FunctionDecl *functionDecl);
  void rememberReturnSource(const FunctionDecl *functionDecl, FunctionDecl *source);


  bool isReturningValue(const Stmt *body) const;
  bool isInferring(const FuncSymbol *function) const;
  bool isSuspended(const FuncSymbol *function) const;
  bool hasReturnSource(const FunctionDecl *functionDecl) const;
  bool hasInheritedReturn(const FunctionDecl *functionDecl) const;


  Checkpoint takeSavedBody();
  CollectedReturns takeCollection();
  CollectedReturns &openCollection();
  FunctionDecl *findReturnSource(const FunctionDecl *functionDecl) const;

private:
  bool isGivingValue(const ReturnStmt *returnStmt) const;

  std::vector<const FuncSymbol *> inferring;
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended;
  llvm::SmallPtrSet<const FunctionDecl *, 4> inheritedReturns;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> returnSources;
  std::vector<llvm::SmallPtrSet<const FuncSymbol *, 4>> suspendedBeforeLoads;
  std::vector<Checkpoint> savedBodies;
  std::deque<CollectedReturns> collections;
};

}
