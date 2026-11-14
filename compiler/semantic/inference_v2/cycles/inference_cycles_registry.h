#pragma once

#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/StringSet.h"
#include <optional>
#include <vector>

namespace tz {

class InferenceCyclesRegistry {
public:
  explicit InferenceCyclesRegistry(SemanticState &owner);

  void findProgramCycles();
  void findCallCycles(const FindCallCycles &task);
  void findMethodCycles(const FindMethodCycles &task);

private:
  enum class CallLinks {
    BY_NAME,
    THROUGH_IMPORTS,
    MEMBERS_ONLY,
  };

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

  struct NodeIndex {
    llvm::StringMap<int> byName;
    llvm::DenseMap<const FunctionDecl *, int> byDecl;
  };

  void closeIfRoot(int node, CycleSearch &search);
  void tarjanSearch(int start, CycleSearch &search);
  void finishTop(CycleSearch &search, std::vector<TarjanFrame> &frames);
  void searchCycles(llvm::ArrayRef<FunctionDecl *> functions, CallLinks links);
  void enter(int node, CycleSearch &search, std::vector<TarjanFrame> &frames) const;
  void followCall(int from, int to, CycleSearch &search, std::vector<TarjanFrame> &frames) const;


  bool isReturnUnknown(const FunctionDecl *functionDecl) const;


  llvm::StringSet<> namesUsed(const FunctionDecl *functionDecl) const;
  llvm::StringSet<> memberCallsIn(const FunctionDecl *functionDecl) const;


  std::optional<int> nodeNamed(const NodeIndex &index, llvm::StringRef name) const;
  std::optional<int> takeNextCall(TarjanFrame &frame, const CycleSearch &search) const;


  NodeIndex indexNodes(llvm::ArrayRef<FunctionDecl *> nodes) const;
  std::vector<FuncSymbol *> popCycle(int node, CycleSearch &search) const;
  std::vector<FunctionDecl *> unknownReturns(llvm::ArrayRef<FunctionDecl *> functions) const;
  std::vector<std::vector<int>> callsBetween(llvm::ArrayRef<FunctionDecl *> nodes,
                                             CallLinks links) const;
  std::optional<int> importedNode(const NodeIndex &index, FunctionDecl *functionDecl,
                                  llvm::StringRef name) const;
  std::optional<int> calledNode(const NodeIndex &index, FunctionDecl *functionDecl,
                                llvm::StringRef name, CallLinks links) const;
  std::vector<int> callsFrom(llvm::ArrayRef<FunctionDecl *> nodes, const NodeIndex &index,
                             FunctionDecl *functionDecl, CallLinks links) const;

  FunctionDecl *cycleCallee(FunctionDecl *functionDecl, llvm::StringRef name) const;
  std::optional<ScopeEntry> exportedValue(Module *source, llvm::StringRef name) const;

  SemanticState &state;
};

}
