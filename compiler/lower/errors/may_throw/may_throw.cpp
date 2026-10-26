#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/errors/throw_scanner/throw_scanner.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

MayThrow::MayThrow(Lowering &lowering) : lowering_(lowering) {}

void MayThrow::analyzeThrows(SourceFile &file) {
  for (Stmt *item : file.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item);
        cls && cls->builtin && !cls->parent && cls->name == "Error") {
      lowering_.errorClass_ = cls;
    }
  }

  std::vector<const FunctionDecl *> functions;
  std::map<const FunctionDecl *, ThrowSummary> summaries;
  std::vector<const FunctionDecl *> work;
  auto add = [&](const FunctionDecl *fn) {
    if (!fn || (fn->cls && fn->cls->builtin) || summaries.count(fn)) {
      return;
    }
    summaries[fn];
    functions.push_back(fn);
    work.push_back(fn);
  };
  ThrowSummary topLevel;
  ThrowScanner topScanner(topLevel);
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        add(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
          add(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          add(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          add(fn);
        }
        add(cls->init);
        for (const ClassField &f : cls->fields) {
          add(f.method);
        }
      }
    }
    topScanner.stmt(item, false);
  }
  for (const FunctionDecl *fn : file.instances) {
    add(fn);
  }
  for (const FunctionDecl *fn : topLevel.nested) {
    add(fn);
  }
  while (!work.empty()) {
    const FunctionDecl *fn = work.back();
    work.pop_back();
    ThrowSummary summary;
    ThrowScanner scanner(summary);
    for (const Param &param : fn->params) {
      scanner.expr(param.defaultValue, false);
    }
    scanner.stmt(fn->body, false);
    if (fn->external) {
      summary.intrinsic = true;
    }
    if (fn->isConstructor) {
      for (const ClassDecl *c = fn->cls; c; c = c->parent) {
        if (c->init && c->init != fn) {
          summary.calls.push_back(c->init);
          summary.allCalls.push_back({c->init, nullptr});
        }
      }
    }
    for (const FunctionDecl *nested : summary.nested) {
      add(nested);
    }
    summaries[fn] = std::move(summary);
    if (fn->isClosure() && !fn->isGenerator && fn->sym && fn->sym->ret) {
      lowering_.tailCallLowerer().findTailGroups(const_cast<FunctionDecl *>(fn));
    }
  }

  for (const FunctionDecl *fn : functions) {
    if (fn->impl) {
      summaries[fn].valueCalls.push_back(fn->impl->type);
      summaries[fn].allValueCalls.push_back(fn->impl->type);
      lowering_.throwingTypes_.insert(fn->impl->type);
    }
  }

  llvm::DenseMap<const Type *, std::vector<const FunctionDecl *>> valuesOf;
  std::vector<std::pair<const Type *, const Type *>> adapters;
  auto collect = [&](const ThrowSummary &summary) {
    for (const auto &[type, fn] : summary.values) {
      valuesOf[type].push_back(fn);
    }
    adapters.insert(adapters.end(), summary.adapters.begin(), summary.adapters.end());
  };
  collect(topLevel);
  for (const auto &[fn, summary] : summaries) {
    collect(summary);
  }
  for (bool changed = !adapters.empty(); changed;) {
    changed = false;
    for (const auto &[to, from] : adapters) {
      std::vector<const FunctionDecl *> wrapped = valuesOf.lookup(from);
      std::vector<const FunctionDecl *> &mine = valuesOf[to];
      for (const FunctionDecl *fn : wrapped) {
        if (!llvm::is_contained(mine, fn)) {
          mine.push_back(fn);
          changed = true;
        }
      }
    }
  }
  for (const auto &[iface, info] : lowering_.interfaces_) {
    for (const Type *implementer : info.implementers) {
      const ClassDecl *cls = implementer->classDecl();
      if (!cls || cls->literal) {
        continue;
      }
      auto implemented = [&](llvm::StringRef name, const Type *fnType) {
        if (const FunctionDecl *fn =
                lowering_.inheritLowerer().implementingMethod(cls, name, fnType)) {
          forEachImplementation(fn, [&](const FunctionDecl *m) { valuesOf[fnType].push_back(m); });
        }
      };
      for (const Field &f : iface->fields()) {
        if (f.type->isFunction()) {
          implemented(f.name, f.type);
        }
      }
      for (const Overload &overload : iface->overloads()) {
        for (const Type *signature : overload.signatures) {
          implemented(overload.name, signature);
        }
      }
    }
  }

  llvm::DenseMap<const FunctionDecl *, size_t> indexOf;
  for (size_t i = 0; i < functions.size(); ++i) {
    indexOf[functions[i]] = i;
  }
  std::vector<std::vector<size_t>> edges(functions.size());
  for (size_t i = 0; i < functions.size(); ++i) {
    const ThrowSummary &summary = summaries[functions[i]];
    auto edge = [&](const FunctionDecl *to) {
      if (auto found = indexOf.find(to); found != indexOf.end()) {
        edges[i].push_back(found->second);
      }
    };
    for (const auto &[to, site] : summary.allCalls) {
      if (!site || !lowering_.tailJumps_.contains(site)) {
        edge(to);
      }
    }
    for (const Type *type : summary.allValueCalls) {
      if (auto found = valuesOf.find(type); found != valuesOf.end()) {
        for (const FunctionDecl *to : found->second) {
          edge(to);
        }
      }
    }
  }
  std::vector<int> index(functions.size(), -1), low(functions.size(), 0);
  std::vector<bool> onStack(functions.size(), false);
  std::vector<size_t> stack;
  int next = 0;
  std::function<void(size_t)> visit = [&](size_t v) {
    index[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    for (size_t w : edges[v]) {
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
    std::vector<size_t> cycle;
    size_t w;
    do {
      w = stack.back();
      stack.pop_back();
      onStack[w] = false;
      cycle.push_back(w);
    } while (w != v);
    if (cycle.size() == 1 && !llvm::is_contained(edges[v], v)) {
      return;
    }
    for (size_t member : cycle) {
      if (!functions[member]->isGenerator) {
        lowering_.recursive_.insert(functions[member]);
      }
    }
  };
  for (size_t v = 0; v < functions.size(); ++v) {
    if (index[v] < 0) {
      visit(v);
    }
  }

  for (const FunctionDecl *fn : functions) {
    const ThrowSummary &summary = summaries[fn];
    if ((summary.intrinsic || summary.resumes || lowering_.recursive_.contains(fn)) &&
        !fn->isAsync) {
      lowering_.throwing_.insert(fn);
    }
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto &[type, fns] : valuesOf) {
      if (!lowering_.throwingTypes_.contains(type) &&
          llvm::any_of(fns,
                       [&](const FunctionDecl *fn) { return lowering_.throwing_.contains(fn); })) {
        lowering_.throwingTypes_.insert(type);
        changed = true;
      }
    }
    for (const FunctionDecl *fn : functions) {
      if (lowering_.throwing_.contains(fn) || fn->isAsync) {
        continue;
      }
      const ThrowSummary &summary = summaries[fn];
      if (llvm::any_of(summary.calls,
                       [&](const FunctionDecl *to) { return lowering_.throwing_.contains(to); }) ||
          llvm::any_of(summary.valueCalls,
                       [&](const Type *type) { return lowering_.typeThrows(type); })) {
        lowering_.throwing_.insert(fn);
        changed = true;
      }
    }
  }
}

bool MayThrow::callMayThrow(const FunctionDecl *fn, bool virtualCall) const {
  if (!virtualCall) {
    return lowering_.mayThrow(fn);
  }
  bool throws = false;
  forEachImplementation(fn, [&](const FunctionDecl *impl) { throws |= lowering_.mayThrow(impl); });

  return throws;
}

bool MayThrow::summaryThrows(const ThrowSummary &summary) const {
  return summary.intrinsic || summary.resumes ||
         llvm::any_of(summary.calls,
                      [&](const FunctionDecl *fn) { return lowering_.mayThrow(fn); }) ||
         llvm::any_of(summary.valueCalls,
                      [&](const Type *type) { return lowering_.typeThrows(type); });
}

bool MayThrow::exprThrows(const Expr *expr) {
  if (!expr) {
    return false;
  }
  if (auto found = exprThrows_.find(expr); found != exprThrows_.end()) {
    return found->second;
  }
  ThrowSummary summary;
  ThrowScanner(summary).expr(expr, false);
  bool throws = summaryThrows(summary);
  exprThrows_[expr] = throws;

  return throws;
}

}
