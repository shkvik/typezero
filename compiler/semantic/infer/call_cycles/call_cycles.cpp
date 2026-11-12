#include "compiler/semantic/infer/call_cycles/call_cycles.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;

namespace tz {

CallCycles::CallCycles(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->importBinder = &all.importBinder;
}

void CallCycles::findCycles(llvm::ArrayRef<FunctionDecl *> functions) {
  this->searchCycles(functions, CallLinks::BY_NAME);
}

void CallCycles::findMethodCycles(llvm::ArrayRef<FunctionDecl *> methods) {
  this->searchCycles(methods, CallLinks::MEMBERS_ONLY);
}

void CallCycles::findProgramCycles() {
  auto declared = this->state.getReturnInference().getDeclaredFunctions();
  this->searchCycles(declared, CallLinks::THROUGH_IMPORTS);
}

void CallCycles::searchCycles(llvm::ArrayRef<FunctionDecl *> functions, CallLinks links) {
  CycleSearch search;
  search.nodes = this->unknownReturns(functions);
  search.calls = this->callsBetween(search.nodes, links);
  search.index.assign(search.nodes.size(), -1);
  search.low.assign(search.nodes.size(), 0);
  search.onStack.assign(search.nodes.size(), false);
  int count = int(search.nodes.size());
  for (int node = 0; node < count; ++node) {
    if (search.index[node] < 0) {
      this->tarjanSearch(node, search);
    }
  }
}

bool CallCycles::isReturnUnknown(const FunctionDecl *functionDecl) const {
  if (functionDecl->sym == nullptr || functionDecl->sym->hasReturnType()) {
    return false;
  }
  const auto &classes = this->state.getClassStore();
  bool isInherited = classes.findInheritedReturn(functionDecl) != nullptr;

  return !isInherited && !classes.hasReturnSource(functionDecl);
}

std::vector<FunctionDecl *>
CallCycles::unknownReturns(llvm::ArrayRef<FunctionDecl *> functions) const {
  auto nodes = std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : functions) {
    if (this->isReturnUnknown(functionDecl)) {
      nodes.push_back(functionDecl);
    }
  }

  return nodes;
}

llvm::StringSet<> CallCycles::memberCallsIn(const FunctionDecl *functionDecl) const {
  auto nodes = PlacedNodes::collect({functionDecl->body}, {});
  auto names = llvm::StringSet<>();
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *call = dyn_cast<CallExpr>(placed.expr);
    auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
    if (member != nullptr) {
      names.insert(member->name);
    }
  }

  return names;
}

llvm::StringSet<> CallCycles::namesUsed(const FunctionDecl *functionDecl) const {
  auto names = llvm::StringSet<>();
  for (const Param &param : functionDecl->params) {
    if (param.defaultValue() != nullptr) {
      AstWalker::collectNames(param.defaultValue(), names);
    }
  }
  AstWalker::collectNames(functionDecl->body, names);

  return names;
}

CallCycles::NodeIndex CallCycles::indexNodes(llvm::ArrayRef<FunctionDecl *> nodes) const {
  auto index = NodeIndex{.byName = {}, .byDecl = {}};
  for (int node = 0; node < int(nodes.size()); ++node) {
    index.byName[nodes[node]->name] = node;
    index.byDecl[nodes[node]] = node;
  }

  return index;
}

std::vector<std::vector<int>> CallCycles::callsBetween(llvm::ArrayRef<FunctionDecl *> nodes,
                                                       CallLinks links) const {
  auto index = this->indexNodes(nodes);
  auto calls = std::vector<std::vector<int>>();
  for (FunctionDecl *functionDecl : nodes) {
    auto targets = this->callsFrom(nodes, index, functionDecl, links);
    calls.push_back(std::move(targets));
  }

  return calls;
}

std::vector<int> CallCycles::callsFrom(llvm::ArrayRef<FunctionDecl *> nodes, const NodeIndex &index,
                                       FunctionDecl *functionDecl, CallLinks links) const {
  auto targets = std::vector<int>();
  auto names = this->namesUsed(functionDecl);
  auto members = this->memberCallsIn(functionDecl);
  for (const auto &member : members) {
    for (int to = 0; to < int(nodes.size()); ++to) {
      if (nodes[to]->cls() != nullptr && nodes[to]->name == member.getKey()) {
        targets.push_back(to);
      }
    }
  }
  for (const auto &name : names) {
    auto target = this->calledNode(index, functionDecl, name.getKey(), links);
    if (target.has_value()) {
      targets.push_back(*target);
    }
  }

  return targets;
}

std::optional<int> CallCycles::calledNode(const NodeIndex &index, FunctionDecl *functionDecl,
                                          llvm::StringRef name, CallLinks links) const {
  switch (links) {
    case CallLinks::BY_NAME:
      return this->nodeNamed(index, name);
    case CallLinks::THROUGH_IMPORTS:
      return this->importedNode(index, functionDecl, name);
    case CallLinks::MEMBERS_ONLY:
      return std::nullopt;
  }

  return std::nullopt;
}

std::optional<int> CallCycles::nodeNamed(const NodeIndex &index, llvm::StringRef name) const {
  auto found = index.byName.find(name);
  if (found == index.byName.end()) {
    return std::nullopt;
  }

  return found->second;
}

std::optional<int> CallCycles::importedNode(const NodeIndex &index, FunctionDecl *functionDecl,
                                            llvm::StringRef name) const {
  FunctionDecl *target = this->importBinder->cycleCallee(functionDecl, name);
  if (target == nullptr) {
    return std::nullopt;
  }
  auto found = index.byDecl.find(target);
  if (found == index.byDecl.end()) {
    return std::nullopt;
  }

  return found->second;
}

void CallCycles::tarjanSearch(int start, CycleSearch &search) {
  auto frames = std::vector<TarjanFrame>();
  this->enter(start, search, frames);
  while (!frames.empty()) {
    int caller = frames.back().node;
    auto callee = this->takeNextCall(frames.back(), search);
    if (callee.has_value()) {
      this->followCall(caller, *callee, search, frames);
    } else {
      this->finishTop(search, frames);
    }
  }
}

std::optional<int> CallCycles::takeNextCall(TarjanFrame &frame, const CycleSearch &search) const {
  const std::vector<int> &calls = search.calls[frame.node];
  if (frame.nextCall == calls.size()) {
    return std::nullopt;
  }
  int callee = calls[frame.nextCall];
  frame.nextCall += 1;

  return callee;
}

void CallCycles::finishTop(CycleSearch &search, std::vector<TarjanFrame> &frames) {
  int finished = frames.back().node;
  frames.pop_back();
  if (!frames.empty()) {
    int caller = frames.back().node;
    search.low[caller] = std::min(search.low[caller], search.low[finished]);
  }
  this->closeIfRoot(finished, search);
}

void CallCycles::enter(int node, CycleSearch &search, std::vector<TarjanFrame> &frames) const {
  search.index[node] = search.next;
  search.low[node] = search.next;
  search.next += 1;
  search.stack.push_back(node);
  search.onStack[node] = true;
  frames.push_back(TarjanFrame{.node = node, .nextCall = 0});
}

void CallCycles::followCall(int from, int to, CycleSearch &search,
                            std::vector<TarjanFrame> &frames) const {
  if (search.index[to] < 0) {
    return this->enter(to, search, frames);
  }
  if (search.onStack[to]) {
    search.low[from] = std::min(search.low[from], search.index[to]);
  }
}

void CallCycles::closeIfRoot(int node, CycleSearch &search) {
  if (search.low[node] != search.index[node]) {
    return;
  }
  auto cycle = this->popCycle(node, search);
  bool isCallingItself = llvm::is_contained(search.calls[node], node);
  if (cycle.size() == 1 && !isCallingItself) {
    return;
  }
  this->state.getReturnInference().addCycle(std::move(cycle));
}

std::vector<FuncSymbol *> CallCycles::popCycle(int node, CycleSearch &search) const {
  auto cycle = std::vector<FuncSymbol *>();
  int member = -1;
  do {
    member = search.stack.back();
    search.stack.pop_back();
    search.onStack[member] = false;
    cycle.push_back(search.nodes[member]->sym);
  } while (member != node);

  return cycle;
}

}
