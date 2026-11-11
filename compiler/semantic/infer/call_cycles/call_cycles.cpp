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

void CallCycles::findCycles(llvm::ArrayRef<FunctionDecl *> functions, ResolveCall resolve) {
  CycleSearch search;
  search.nodes = this->unknownReturns(functions);
  search.calls = this->callsBetween(search.nodes, resolve);
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

void CallCycles::findProgramCycles() {
  ImportBinder &binder = *this->importBinder;
  auto callee = [&binder](FunctionDecl *fn, llvm::StringRef name) {
    return binder.cycleCallee(fn, name);
  };
  std::vector<FunctionDecl *> declared = this->state.getReturnInference().getDeclaredFunctions();
  this->findCycles(declared, callee);
}

std::vector<FunctionDecl *>
CallCycles::unknownReturns(llvm::ArrayRef<FunctionDecl *> functions) const {
  std::vector<FunctionDecl *> nodes;
  for (FunctionDecl *fn : functions) {
    bool isUnknown = fn->sym && !fn->sym->hasReturnType() &&
                     !this->state.getClassStore().findInheritedReturn(fn) &&
                     !this->state.getClassStore().hasReturnSource(fn);
    if (isUnknown) {
      nodes.push_back(fn);
    }
  }

  return nodes;
}

llvm::StringSet<> CallCycles::memberCallsIn(const FunctionDecl *fn) {
  PlacedNodes nodes = PlacedNodes::collect({fn->body}, {});
  llvm::StringSet<> names;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *call = dyn_cast<CallExpr>(placed.expr);
    auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
    if (member != nullptr) {
      names.insert(member->name);
    }
  }

  return names;
}

llvm::StringSet<> CallCycles::namesUsed(const FunctionDecl *fn) {
  llvm::StringSet<> names;
  for (const Param &param : fn->params) {
    if (param.defaultValue()) {
      AstWalker::collectNames(param.defaultValue(), names);
    }
  }
  AstWalker::collectNames(fn->body, names);

  return names;
}

std::vector<std::vector<int>> CallCycles::callsBetween(llvm::ArrayRef<FunctionDecl *> nodes,
                                                       ResolveCall resolve) const {
  int count = int(nodes.size());
  llvm::StringMap<int> byName;
  llvm::DenseMap<const FunctionDecl *, int> byDecl;
  for (int node = 0; node < count; ++node) {
    byName[nodes[node]->name] = node;
    byDecl[nodes[node]] = node;
  }

  std::vector<std::vector<int>> calls(nodes.size());
  for (int from = 0; from < count; ++from) {
    FunctionDecl *fn = nodes[from];
    llvm::StringSet<> names = CallCycles::namesUsed(fn);
    llvm::StringSet<> members = CallCycles::memberCallsIn(fn);
    for (const auto &member : members) {
      for (int to = 0; to < count; ++to) {
        if (nodes[to]->cls() && nodes[to]->name == member.getKey()) {
          calls[from].push_back(to);
        }
      }
    }
    for (const auto &name : names) {
      llvm::StringRef key = name.getKey();
      if (!resolve) {
        auto sameName = byName.find(key);
        if (sameName != byName.end()) {
          calls[from].push_back(sameName->second);
        }
        continue;
      }
      FunctionDecl *target = resolve(fn, key);
      auto sameDecl = target ? byDecl.find(target) : byDecl.end();
      if (sameDecl != byDecl.end()) {
        calls[from].push_back(sameDecl->second);
      }
    }
  }

  return calls;
}

void CallCycles::tarjanSearch(int start, CycleSearch &search) {
  std::vector<TarjanFrame> frames;
  CallCycles::enter(start, search, frames);
  while (!frames.empty()) {
    int caller = frames.back().node;
    std::optional<int> callee = CallCycles::takeNextCall(frames.back(), search);
    if (callee.has_value()) {
      CallCycles::followCall(caller, *callee, search, frames);
    } else {
      this->finishTop(search, frames);
    }
  }
}

std::optional<int> CallCycles::takeNextCall(TarjanFrame &frame, const CycleSearch &search) {
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

void CallCycles::enter(int node, CycleSearch &search, std::vector<TarjanFrame> &frames) {
  search.index[node] = search.next;
  search.low[node] = search.next;
  search.next += 1;
  search.stack.push_back(node);
  search.onStack[node] = true;
  frames.push_back(TarjanFrame{.node = node, .nextCall = 0});
}

void CallCycles::followCall(int from, int to, CycleSearch &search,
                            std::vector<TarjanFrame> &frames) {
  if (search.index[to] < 0) {
    CallCycles::enter(to, search, frames);
  } else if (search.onStack[to]) {
    search.low[from] = std::min(search.low[from], search.index[to]);
  }
}

void CallCycles::closeIfRoot(int node, CycleSearch &search) {
  if (search.low[node] != search.index[node]) {
    return;
  }
  std::vector<FuncSymbol *> cycle = CallCycles::popCycle(node, search);
  bool isCallingItself = llvm::is_contained(search.calls[node], node);
  if (cycle.size() == 1 && !isCallingItself) {
    return;
  }
  this->state.getReturnInference().addCycle(std::move(cycle));
}

std::vector<FuncSymbol *> CallCycles::popCycle(int node, CycleSearch &search) {
  std::vector<FuncSymbol *> cycle;
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
