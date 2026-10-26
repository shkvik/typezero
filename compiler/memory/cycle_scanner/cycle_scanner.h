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

struct CycleGraph {
  enum Kind { TypeNode, EnvNode, BoxNode, FnNode, AnyObjectNode, UnknownNode };
  struct Node {
    Kind kind;
    const void *key;
    std::vector<std::pair<unsigned, bool>> edges;
  };
  std::vector<Node> nodes;
  std::map<std::pair<int, const void *>, unsigned> index;

  unsigned node(Kind kind, const void *key) {
    auto [it, inserted] = index.try_emplace({int(kind), key}, unsigned(nodes.size()));
    if (inserted) {
      nodes.push_back({kind, key, {}});
    }

    return it->second;
  }
  void edge(unsigned from, unsigned to, bool late) { nodes[from].edges.push_back({to, late}); }
};

class CycleExprs;
class CycleEscapes;

class CycleScanner {
public:
  std::vector<const Type *> types;
  llvm::DenseSet<const Type *> seenTypes;
  std::set<std::string> mutableFields;
  std::map<const void *, std::vector<const VarSymbol *>> envs;
  llvm::DenseSet<const VarSymbol *> boxed;
  std::map<const Type *, std::set<const void *>> closuresOf;
  std::map<const Type *, std::set<const Type *>> boundOf;
  std::set<std::pair<const Type *, const Type *>> fnCasts;
  std::set<const Type *> reactionCallbacks;
  llvm::DenseSet<const Type *> mutated;
  llvm::DenseSet<const Type *> toUnknown;
  void addType(const Type *type);
  void addVar(const VarSymbol *var);
  void function(const FunctionDecl *fn);
  void classDecl(const ClassDecl *cls);
  void stmt(const Stmt *stmt);

private:
  static const Type *functionType(const FunctionDecl *fn);

public:
  static bool thisEscapes(const Stmt *stmt, const VarSymbol *self);

private:
  llvm::DenseSet<const FunctionDecl *> scanned_;
  llvm::DenseSet<const Expr *> callee_;
  llvm::DenseMap<const ClassDecl *, bool> escapes_;
  std::map<const FunctionDecl *, llvm::DenseSet<const AssignExpr *>> safeWrites_;
  const FunctionDecl *current_ = nullptr;

public:
  CycleScanner();
  ~CycleScanner();

  CycleExprs &cycleExprs() const { return *cycleExprsPart_; }
  CycleEscapes &cycleEscapes() const { return *cycleEscapesPart_; }

private:
  friend class CycleExprs;
  friend class CycleEscapes;
  std::unique_ptr<CycleExprs> cycleExprsPart_;
  std::unique_ptr<CycleEscapes> cycleEscapesPart_;
};

}
