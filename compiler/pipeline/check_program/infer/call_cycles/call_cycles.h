#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include <vector>

namespace tz {

class CallCycles {
public:
  using ResolveCall = llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)>;

  explicit CallCycles(CheckState &owner);

  void findCycles(llvm::ArrayRef<FunctionDecl *> functions, ResolveCall resolve = nullptr);

private:
  struct CycleSearch {
    int next = 0;
    std::vector<int> low;
    std::vector<int> index;
    std::vector<int> stack;
    std::vector<bool> onStack;
    std::vector<FunctionDecl *> nodes;
    std::vector<std::vector<int>> calls;
  };

  static llvm::StringSet<> namesUsed(const FunctionDecl *fn);
  static std::vector<FuncSymbol *> popCycle(int node, CycleSearch &search);

  void visit(int node, CycleSearch &search);
  std::vector<FunctionDecl *> unknownReturns(llvm::ArrayRef<FunctionDecl *> functions) const;
  std::vector<std::vector<int>> callsBetween(llvm::ArrayRef<FunctionDecl *> nodes,
                                             ResolveCall resolve) const;

  CheckState &state;
};

}
