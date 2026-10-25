#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace tz {

// Which stores of references put values into groups.

// What a call through the method table of `fn` may run: fn and the methods
// that override it down the chain (see codegen_throw.cpp).
void forEachOverride(const FunctionDecl *fn, llvm::function_ref<void(const FunctionDecl *)> visit);

// Layer 1 of docs/objects.md, section 11: the objects that are never stored
// anywhere a reference can be kept from (a field, an element, an entry, an
// environment, a box), only in variables and arguments, cannot be on a
// cycle. They are counted as any other object, and a reference to a member
// stored in them does not join groups: the arrays a search keeps its work
// in do not stay in the group of the graph it searches.
//
// The analysis follows values through variables, arguments, `this` and
// results, over the whole program: a node of its graph is a place a value
// may be (a variable, the result of a function, an allocation), and an edge
// says that if what is at its start is stored, so is what is at its end.
// Whatever reaches a place that stores is stored; an allocation nothing
// stored reaches is not.
class StoreStmts;
class StoreExprs;
class StoreCalls;

class StoreScanner {
public:
  // `site` tells the allocations that matter: those of types that may form
  // cycles.
  explicit StoreScanner(std::function<bool(const Expr *)> site);
  void function(const FunctionDecl *fn);
  // Top-level code: no function, no result.
  void topLevel(const Stmt *s);
  // A function used as a value: whoever calls it may keep what it returns.
  void escapes(const FunctionDecl *fn);
  // The allocations nothing stored reaches.
  llvm::DenseSet<const Expr *> unstored();

private:
  enum class Kind { Stored, Var, Result, Site };
  struct Key {
    Kind kind;
    const void *ptr;
    bool operator<(const Key &o) const { return kind != o.kind ? kind < o.kind : ptr < o.ptr; }
  };
  using Sources = std::vector<unsigned>;
  unsigned node(Key key);
  unsigned stored();
  unsigned var(const VarSymbol *v);
  unsigned result(const FunctionDecl *fn);
  void edge(unsigned from, unsigned to);
  void flowInto(unsigned place, const Sources &values);
  void store(const Sources &values);
  std::function<bool(const Expr *)> isSite_;
  std::map<Key, unsigned> index_;
  std::vector<std::vector<unsigned>> edges_;
  llvm::DenseSet<const FunctionDecl *> scanned_;
  llvm::DenseSet<const Expr *> callee_;
  const FunctionDecl *current_ = nullptr;

public:
  ~StoreScanner();

  StoreStmts &storeStmts() const { return *storeStmtsPart_; }
  StoreExprs &storeExprs() const { return *storeExprsPart_; }
  StoreCalls &storeCalls() const { return *storeCallsPart_; }

private:
  friend class StoreStmts;
  friend class StoreExprs;
  friend class StoreCalls;
  std::unique_ptr<StoreStmts> storeStmtsPart_;
  std::unique_ptr<StoreExprs> storeExprsPart_;
  std::unique_ptr<StoreCalls> storeCallsPart_;
};

} // namespace tz
