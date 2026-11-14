#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

class ReturnInferenceContext {
public:
  void leaveInference();
  void enterModuleLoad();
  void leaveModuleLoad();
  void enterInference(const FuncSymbol *function);
  void forgetSuspended(const FuncSymbol *function);
  void rememberInheritedReturn(const FunctionDecl *functionDecl);
  void rememberReturnSource(const FunctionDecl *functionDecl, FunctionDecl *source);


  bool isInferring(const FuncSymbol *function) const;
  bool isSuspended(const FuncSymbol *function) const;
  bool hasReturnSource(const FunctionDecl *functionDecl) const;
  bool hasInheritedReturn(const FunctionDecl *functionDecl) const;


  FunctionDecl *findReturnSource(const FunctionDecl *functionDecl) const;

private:
  std::vector<const FuncSymbol *> inferring;
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended;
  llvm::SmallPtrSet<const FunctionDecl *, 4> inheritedReturns;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> returnSources;
  std::vector<llvm::SmallPtrSet<const FuncSymbol *, 4>> suspendedBeforeLoads;
};

}
