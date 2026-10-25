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

// Which types of the program may form cycles of references.

// The nodes of the graph: types whose objects hold references (objects,
// arrays, dictionaries), environments, boxes, and three that stand for what
// a value may refer to: any function's environment, any object (in generic
// code) and what an unknown may hold.
struct CycleGraph {
  enum Kind { TypeNode, EnvNode, BoxNode, FnNode, AnyObjectNode, UnknownNode };
  struct Node {
    Kind kind;
    const void *key;
    std::vector<std::pair<unsigned, bool>> edges; // target, may be set after it is made
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
  // Both ends are found first: making a node may move the others.
  void edge(unsigned from, unsigned to, bool late) { nodes[from].edges.push_back({to, late}); }
};

// Walks the code of the program and gathers what the analysis needs.
class CycleExprs;
class CycleEscapes;

class CycleScanner {
public:
  // The types the program has values of, with the types they are made of.
  std::vector<const Type *> types;
  llvm::DenseSet<const Type *> seenTypes;
  // The fields that may be assigned after their object is made, by name.
  std::set<std::string> mutableFields;
  // The closures with an environment on the heap (by the key of their
  // environment: their group, or themselves) and what each captures.
  std::map<const void *, std::vector<const VarSymbol *>> envs;
  // The variables that live in boxes.
  llvm::DenseSet<const VarSymbol *> boxed;
  // The function values the program makes, by their types: the closures
  // (by the keys of their environments), and the static types of objects
  // whose methods are taken as values (the object is the environment). A
  // value of a type converted to another is one of that too.
  std::map<const Type *, std::set<const void *>> closuresOf;
  std::map<const Type *, std::set<const Type *>> boundOf;
  std::set<std::pair<const Type *, const Type *>> fnCasts; // from, to
  // The types of the callbacks given to promises and ResultAsync, which a
  // reaction holds (Lowering::attachReaction).
  std::set<const Type *> reactionCallbacks;
  // The arrays and dictionaries that code adds to after they are made (by
  // static type): only their elements may close a cycle.
  llvm::DenseSet<const Type *> mutated;
  // The types of values that become unknowns (the objects among them are
  // held as they are).
  llvm::DenseSet<const Type *> toUnknown;
  void addType(const Type *type);
  void addVar(const VarSymbol *var);
  // A function and what it declares, once.
  void function(const FunctionDecl *fn);
  // A class's static fields and decorations, where it is declared.
  void classDecl(const ClassDecl *cls);
  void stmt(const Stmt *stmt);

private:
  static const Type *functionType(const FunctionDecl *fn);

public:
  // Whether `self` is used in `stmt` for anything but a field: as an
  // argument, a value, a method's object, or captured by a closure.
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

} // namespace tz
