#include "compiler/memory/store_scanner/store_scanner.h"
#include "compiler/memory/store_calls/store_calls.h"
#include "compiler/memory/store_exprs/store_exprs.h"
#include "compiler/memory/store_stmts/store_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <map>
#include <set>

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

void forEachOverride(const FunctionDecl *fn, llvm::function_ref<void(const FunctionDecl *)> visit) {
  if (!fn->isAbstract) {
    visit(fn);
  }
  if (!fn->cls || fn->slot < 0) {
    return;
  }
  std::function<void(const ClassDecl *)> down = [&](const ClassDecl *cls) {
    for (const ClassDecl *child : cls->children) {
      if (size_t(fn->slot) < child->vtable.size()) {
        if (const FunctionDecl *impl = child->vtable[size_t(fn->slot)]; impl && impl != fn) {
          visit(impl);
        }
      }
      down(child);
    }
  };
  down(fn->cls);
}

StoreScanner::StoreScanner(std::function<bool(const Expr *)> site) : isSite_(std::move(site)) {
  storeStmtsPart_ = std::make_unique<StoreStmts>(*this);
  storeExprsPart_ = std::make_unique<StoreExprs>(*this);
  storeCallsPart_ = std::make_unique<StoreCalls>(*this);
  node(Key{Kind::Stored, nullptr});
}

void StoreScanner::function(const FunctionDecl *fn) {
  if (!fn || !scanned_.insert(fn).second) {
    return;
  }
  const FunctionDecl *saved = current_;
  current_ = fn;
  for (const Param &param : fn->params) {
    if (param.defaultValue) {
      flowInto(var(param.sym), storeExprs().expr(param.defaultValue));
    }
  }
  if (fn->sym && fn->sym->hasRest && !fn->params.empty()) {
    flowInto(var(fn->params.back().sym), {});
  }
  storeStmts().stmt(fn->body);
  current_ = saved;
}

void StoreScanner::topLevel(const Stmt *s) {
  current_ = nullptr;
  storeStmts().stmt(s);
}

void StoreScanner::escapes(const FunctionDecl *fn) {
  if (fn) {
    edge(stored(), result(fn));
  }
}

llvm::DenseSet<const Expr *> StoreScanner::unstored() {
  std::vector<bool> reached(edges_.size(), false);
  std::vector<unsigned> work{0};
  reached[0] = true;
  while (!work.empty()) {
    unsigned n = work.back();
    work.pop_back();
    for (unsigned m : edges_[n]) {
      if (!reached[m]) {
        reached[m] = true;
        work.push_back(m);
      }
    }
  }
  llvm::DenseSet<const Expr *> result;
  for (const auto &[key, n] : index_) {
    if (key.kind == Kind::Site && !reached[n]) {
      result.insert(static_cast<const Expr *>(key.ptr));
    }
  }

  return result;
}

unsigned StoreScanner::node(Key key) {
  auto [it, inserted] = index_.try_emplace(key, unsigned(edges_.size()));
  if (inserted) {
    edges_.emplace_back();
  }

  return it->second;
}

unsigned StoreScanner::stored() { return 0; }

unsigned StoreScanner::var(const VarSymbol *v) {
  unsigned n = node({Kind::Var, v});
  if (v && v->boxed()) {
    edge(stored(), n);
  }

  return n;
}

unsigned StoreScanner::result(const FunctionDecl *fn) { return node({Kind::Result, fn}); }

void StoreScanner::edge(unsigned from, unsigned to) {
  if (from != to) {
    edges_[from].push_back(to);
  }
}

void StoreScanner::flowInto(unsigned place, const Sources &values) {
  for (unsigned v : values) {
    edge(place, v);
  }
}

void StoreScanner::store(const Sources &values) { flowInto(stored(), values); }

StoreScanner::~StoreScanner() = default;
}
