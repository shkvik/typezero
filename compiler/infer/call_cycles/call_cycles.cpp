#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/declare/function_names/function_names.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

CallCycles::CallCycles(Checker &checker) : checker_(checker) {}

void CallCycles::findCycles(
    llvm::ArrayRef<FunctionDecl *> functions,
    llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)> resolve) {
  // Tarjan's algorithm over the calls between functions without a return
  // type. A name that only looks like such a call (a variable with the same
  // name) costs a few more checks, nothing else.
  std::vector<FunctionDecl *> nodes;
  llvm::StringMap<size_t> byName;
  llvm::DenseMap<const FunctionDecl *, size_t> byDecl;
  for (FunctionDecl *fn : functions) {
    if (fn->sym && !fn->sym->ret && !checker_.inheritsReturn_.count(fn) &&
        !checker_.returnSources_.count(fn)) {
      byName[fn->name] = nodes.size();
      byDecl[fn] = nodes.size();
      nodes.push_back(fn);
    }
  }
  std::vector<std::vector<size_t>> calls(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    llvm::StringSet<> names;
    for (const Param &param : nodes[i]->params) {
      if (param.defaultValue) {
        collectNames(param.defaultValue, names);
      }
    }
    collectNames(nodes[i]->body, names);
    // `o.m(...)` may call any method named m.
    llvm::StringSet<> members;
    collectMemberCalls(nodes[i]->body, members);
    for (const auto &member : members) {
      for (size_t j = 0; j < nodes.size(); ++j) {
        if (nodes[j]->cls && nodes[j]->name == member.getKey()) {
          calls[i].push_back(j);
        }
      }
    }
    for (const auto &name : names) {
      if (!resolve) {
        if (auto found = byName.find(name.getKey()); found != byName.end()) {
          calls[i].push_back(found->second);
        }
      } else if (FunctionDecl *callee = resolve(nodes[i], name.getKey())) {
        if (auto found = byDecl.find(callee); found != byDecl.end()) {
          calls[i].push_back(found->second);
        }
      }
    }
  }

  std::vector<int> index(nodes.size(), -1), low(nodes.size(), 0);
  std::vector<bool> onStack(nodes.size(), false);
  std::vector<size_t> stack;
  int next = 0;
  std::function<void(size_t)> visit = [&](size_t v) {
    index[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    for (size_t w : calls[v]) {
      if (index[w] < 0) {
        visit(w);
        low[v] = std::min(low[v], low[w]);
      } else if (onStack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] != index[v]) {
      return;
    }
    std::vector<FuncSymbol *> cycle;
    size_t w;
    do {
      w = stack.back();
      stack.pop_back();
      onStack[w] = false;
      cycle.push_back(nodes[w]->sym);
    } while (w != v);
    if (cycle.size() == 1 && !llvm::is_contained(calls[v], v)) {
      return; // not recursive
    }
    for (FuncSymbol *fn : cycle) {
      checker_.cycleOf_[fn] = static_cast<unsigned>(checker_.cycles_.size());
    }
    checker_.cycles_.push_back(std::move(cycle));
  };
  for (size_t v = 0; v < nodes.size(); ++v) {
    if (index[v] < 0) {
      visit(v);
    }
  }
}

} // namespace tz
