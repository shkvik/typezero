#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class ImportBinder;

class CallCycles {
public:
  using ResolveCall = llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)>;

  CallCycles(SemanticState &owner, SemanticParts &all);

  void findCycles(llvm::ArrayRef<FunctionDecl *> functions, ResolveCall resolve = nullptr);
  void findProgramCycles();

private:
  struct TarjanFrame {
    int node;
    size_t nextCall;
  };

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
  static llvm::StringSet<> memberCallsIn(const FunctionDecl *fn);
  static std::vector<FuncSymbol *> popCycle(int node, CycleSearch &search);
  static std::optional<int> takeNextCall(TarjanFrame &frame, const CycleSearch &search);
  static void enter(int node, CycleSearch &search, std::vector<TarjanFrame> &frames);
  static void followCall(int from, int to, CycleSearch &search, std::vector<TarjanFrame> &frames);

  void tarjanSearch(int start, CycleSearch &search);
  void finishTop(CycleSearch &search, std::vector<TarjanFrame> &frames);
  void closeIfRoot(int node, CycleSearch &search);
  std::vector<FunctionDecl *> unknownReturns(llvm::ArrayRef<FunctionDecl *> functions) const;
  std::vector<std::vector<int>> callsBetween(llvm::ArrayRef<FunctionDecl *> nodes,
                                             ResolveCall resolve) const;

  SemanticState &state;
  ImportBinder *importBinder = nullptr;
};

}
