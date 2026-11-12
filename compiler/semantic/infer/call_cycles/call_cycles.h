#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class ImportBinder;

class CallCycles {
public:
  CallCycles(SemanticState &owner, SemanticParts &all);

  void findProgramCycles();
  void findCycles(llvm::ArrayRef<FunctionDecl *> functions);
  void findMethodCycles(llvm::ArrayRef<FunctionDecl *> methods);

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

  void tarjanSearch(int start, CycleSearch &search);
  void closeIfRoot(int node, CycleSearch &search);
  bool isReturnUnknown(const FunctionDecl *functionDecl) const;
  llvm::StringSet<> namesUsed(const FunctionDecl *functionDecl) const;
  NodeIndex indexNodes(llvm::ArrayRef<FunctionDecl *> nodes) const;
  void finishTop(CycleSearch &search, std::vector<TarjanFrame> &frames);
  llvm::StringSet<> memberCallsIn(const FunctionDecl *functionDecl) const;
  std::vector<FuncSymbol *> popCycle(int node, CycleSearch &search) const;
  void searchCycles(llvm::ArrayRef<FunctionDecl *> functions, CallLinks links);
  std::optional<int> nodeNamed(const NodeIndex &index, llvm::StringRef name) const;
  std::optional<int> takeNextCall(TarjanFrame &frame, const CycleSearch &search) const;
  void enter(int node, CycleSearch &search, std::vector<TarjanFrame> &frames) const;
  std::vector<FunctionDecl *> unknownReturns(llvm::ArrayRef<FunctionDecl *> functions) const;
  void followCall(int from, int to, CycleSearch &search, std::vector<TarjanFrame> &frames) const;
  std::vector<std::vector<int>> callsBetween(llvm::ArrayRef<FunctionDecl *> nodes,
                                             CallLinks links) const;
  std::optional<int> importedNode(const NodeIndex &index, FunctionDecl *functionDecl,
                                  llvm::StringRef name) const;
  std::optional<int> calledNode(const NodeIndex &index, FunctionDecl *functionDecl,
                                llvm::StringRef name, CallLinks links) const;
  std::vector<int> callsFrom(llvm::ArrayRef<FunctionDecl *> nodes, const NodeIndex &index,
                             FunctionDecl *functionDecl, CallLinks links) const;

  SemanticState &state;
  ImportBinder *importBinder = nullptr;
};

}
