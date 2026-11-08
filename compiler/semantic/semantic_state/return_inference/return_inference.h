#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

class ReturnInference {
public:
  void enterInference(const FuncSymbol *fn);
  void leaveInference();
  void enterModuleLoad();
  void leaveModuleLoad();
  bool isSuspended(const FuncSymbol *fn) const;
  void forgetSuspended(const FuncSymbol *fn);
  void addCycle(std::vector<FuncSymbol *> cycle);
  bool isInCycle(const FuncSymbol *fn) const;
  std::vector<FuncSymbol *> getCycleOf(const FuncSymbol *fn) const;
  void rememberDeclaredFunctions(std::vector<FunctionDecl *> functions);
  std::vector<FunctionDecl *> getDeclaredFunctions() const;
  void addLiftedFunction(FunctionDecl *fn);
  std::vector<FunctionDecl *> getLiftedFunctions() const;
  unsigned takeSequenceNumber();
  unsigned getSequenceNumber() const;

private:
  std::vector<const FuncSymbol *> inferring;
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended;
  std::vector<llvm::SmallPtrSet<const FuncSymbol *, 4>> suspendedBeforeLoads;
  std::vector<std::vector<FuncSymbol *>> cycles;
  llvm::DenseMap<const FuncSymbol *, int> cycleOf;
  std::vector<FunctionDecl *> declaredFunctions;
  std::vector<FunctionDecl *> liftedFunctions;
  unsigned sequenceNumber = 0;
};

}
