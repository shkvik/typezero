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

void forEachOverride(const FunctionDecl *fn, llvm::function_ref<void(const FunctionDecl *)> visit);

class StoreStmts;
class StoreExprs;
class StoreCalls;

class StoreScanner {
public:
  explicit StoreScanner(std::function<bool(const Expr *)> site);
  void function(const FunctionDecl *fn);
  void topLevel(const Stmt *s);
  void escapes(const FunctionDecl *fn);
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

}
