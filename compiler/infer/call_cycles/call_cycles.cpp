#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/declare/function_names/function_names.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

CallCycles::CallCycles(Checker &owner)
    : checker(owner) {}

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
      this->visit(node, search);
    }
  }
}

std::vector<FunctionDecl *>
CallCycles::unknownReturns(llvm::ArrayRef<FunctionDecl *> functions) const {
  std::vector<FunctionDecl *> nodes;
  for (FunctionDecl *fn : functions) {
    bool unknown = fn->sym && !fn->sym->ret && !this->checker.inheritsReturn_.count(fn) &&
                   !this->checker.returnSources_.count(fn);
    if (unknown) {
      nodes.push_back(fn);
    }
  }

  return nodes;
}

llvm::StringSet<> CallCycles::namesUsed(const FunctionDecl *fn) {
  llvm::StringSet<> names;
  for (const Param &param : fn->params) {
    if (param.defaultValue()) {
      collectNames(param.defaultValue(), names);
    }
  }
  collectNames(fn->body, names);

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
    llvm::StringSet<> members;
    collectMemberCalls(fn->body, members);
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

void CallCycles::visit(int node, CycleSearch &search) {
  search.index[node] = search.next;
  search.low[node] = search.next;
  ++search.next;
  search.stack.push_back(node);
  search.onStack[node] = true;
  for (int to : search.calls[node]) {
    if (search.index[to] < 0) {
      this->visit(to, search);
      search.low[node] = std::min(search.low[node], search.low[to]);
    } else if (search.onStack[to]) {
      search.low[node] = std::min(search.low[node], search.index[to]);
    }
  }
  if (search.low[node] != search.index[node]) {
    return;
  }

  std::vector<FuncSymbol *> cycle = CallCycles::popCycle(node, search);
  bool callsItself = llvm::is_contained(search.calls[node], node);
  if (cycle.size() == 1 && !callsItself) {
    return;
  }
  unsigned id = static_cast<unsigned>(this->checker.cycles_.size());
  for (FuncSymbol *fn : cycle) {
    this->checker.cycleOf_[fn] = id;
  }
  this->checker.cycles_.push_back(std::move(cycle));
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
