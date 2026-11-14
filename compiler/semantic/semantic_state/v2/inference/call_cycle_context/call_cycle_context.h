#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/DenseMap.h"
#include <vector>

namespace tz {

class CallCycleContext {
public:
  void addCycle(std::vector<FuncSymbol *> cycle);
  void rememberDeclaredFunctions(std::vector<FunctionDecl *> functions);


  bool isInCycle(const FuncSymbol *function) const;


  std::vector<FunctionDecl *> getDeclaredFunctions() const;
  std::vector<FuncSymbol *> getCycleOf(const FuncSymbol *function) const;

private:
  std::vector<FunctionDecl *> declaredFunctions;
  std::vector<std::vector<FuncSymbol *>> cycles;
  llvm::DenseMap<const FuncSymbol *, int> cycleOf;
};

}
