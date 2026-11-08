#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace tz::parallel {

class Ownership {
public:
  explicit Ownership(Analysis &analysis);

  static bool valueArgument(const Expr *e);
  std::optional<Ref> ownedRef(const Expr *e);
  void decide();
  std::optional<Ref> substituted(Ref r, const FnInfo &callee, int caller, const CallRec &rec,
                                 const CallEdge &call);
  void rewrite();
  bool confined(const FunctionDecl *fn) const;
  bool confinedVar(const VarSymbol *v) const;

private:
  using Roots = llvm::SmallVector<const VarSymbol *, 2>;
  using Needs = std::vector<FieldKey>;
  struct Store {
    const Type *type = nullptr;
    const Expr *value = nullptr;
    int fn = 0;
    const VarSymbol *holder = nullptr;
    const ClassDecl *built = nullptr;
  };
  static constexpr unsigned kSelf = ~0u;
  const VarSymbol *rootOf(const FunctionDecl *fn, const Expr *e, Needs &needs, unsigned depth);
  static bool isParam(const FunctionDecl *fn, const VarSymbol *v);
  static bool isSelf(const FunctionDecl *fn, const VarSymbol *v);
  static bool builds(const FunctionDecl *fn);
  static const std::vector<Expr *> *argsOf(const CallRec &rec);
  static const Expr *receiverOf(const CallRec &rec);
  const CallRec *recOf(const void *site) const;
  bool passesOwnedSelf(const CallExpr *c);
  bool passesOwnedNew(const NewExpr *n, size_t index);
  bool selfOwned(int fi, const CallRec &rec, const CallEdge &callee, bool strict);
  bool candidate(const FunctionDecl *fn, const VarSymbol *v) const;
  static bool tracked(const FunctionDecl *fn, const VarSymbol *v);
  bool freshCandidate(const FunctionDecl *fn, const VarSymbol *v) const;
  bool freshAllocation(const Expr *e) const;
  const CallExpr *programCall(const Expr *e) const;
  static bool libraryField(const Type *object, llvm::StringRef name);
  static bool plainObject(const Type *type);
  bool container(const Type *type);
  void libraryTypes(const Stmt *item, std::set<const Type *> &out);
  static bool plainValues(const Type *type, unsigned depth);
  static bool derivable(const Type *type, unsigned depth);
  void carriers(const Expr *e, Roots &out, unsigned depth);
  void carriersOf(const VarSymbol *v, Roots &out, unsigned depth);
  void carriersOther(const Expr *e, Roots &out, unsigned depth);
  static const FunctionDecl *homeOf(const FunctionDecl *fn, const VarSymbol *v);
  static const std::vector<VarSymbol *> &capturesOf(const FunctionDecl *fn);
  static const FunctionDecl *closureOf(const Expr *e);
  static const Expr *cryptoSelf(const CallExpr *c);
  void closure(const FunctionDecl *fn, bool confined);
  void loops(const FunctionDecl *fn, const Stmt *s);
  llvm::DenseSet<const FunctionDecl *> goodClosures;
  llvm::DenseSet<const FunctionDecl *> badClosures;
  llvm::DenseMap<const VarSymbol *, std::vector<const FunctionDecl *>> capturers;
  void leak(const Expr *e);
  void returnParams(const Expr *e);
  bool returnsBack(const CallExpr *c, size_t index);
  llvm::DenseSet<const VarSymbol *> returned;
  void visitStmt(const Stmt *s);
  void visit(const Expr *e);
  void visitCall(const CallExpr *c);
  void literal(const ObjectLitExpr *o);
  void dirty(const Expr *object, llvm::StringRef field);
  void dirtyAll(const Expr *e);
  bool thisFirst(const CallRec &rec) const;
  bool tailAwaited(int fi);
  bool handsOver(int fi, const CallRec &rec, Mode mode, bool strict);
  int argOf(int fi, const CallRec &rec, const CallEdge &callee, size_t param, bool strict);
  bool passesOwned(const CallExpr *c, size_t index);
  bool reflectKeepsArgs(const CallExpr *c, bool self);
  bool callersOwned(int fi, size_t index, bool strict);
  bool poolFresh(const Type *item) const;
  bool callFresh(int fi, const CallExpr *c) const;
  bool valueOwn(int fi, const Expr *e, bool params, unsigned depth);
  bool fieldOpen(const FieldKey &key);
  bool fieldOwn(const FieldKey &key);
  bool needsOwn(const Needs &needs);
  bool own(const OwnedRef *owned);
  Ref interned(const VarSymbol *root, const Type *type, Needs needs, bool whole);
  void scan(int fi);
  void decideOnce();
  struct SelfStore {
    const VarSymbol *self = nullptr;
    const Expr *value = nullptr;
    int fi = 0;
    Needs needs;
  };
  std::vector<SelfStore> selfStores;
  llvm::DenseSet<const VarSymbol *> forced;
  Analysis &analysis;
  const FunctionDecl *currentFunction = nullptr;
  int currentIndex = 0;
  bool collecting = false;
  const Expr *heldLiteral = nullptr;
  const VarSymbol *currentHolder = nullptr;
  const FunctionDecl *returningIn = nullptr;
  llvm::DenseSet<const VarSymbol *> leaked;
  llvm::DenseSet<const VarSymbol *> leaking;
  llvm::DenseSet<const VarSymbol *> ownedVars;
  llvm::DenseMap<const VarSymbol *, std::pair<const FunctionDecl *, unsigned>> paramOf;
  llvm::DenseMap<const VarSymbol *, int> freshOf;
  llvm::DenseMap<const VarSymbol *, const Expr *> loopInit;
  std::vector<std::vector<const Expr *>> returns;
  std::map<const char *, std::vector<Store>> fieldStores;
  std::map<const char *, std::vector<const Type *>> dirty_;
  std::map<FieldKey, bool> open;
  std::set<FieldKey> asked;
  std::set<FieldKey> evaluated;
  std::set<FieldKey> notOwn;
  bool deciding = false;
  llvm::DenseSet<int> freshIndices;
  std::vector<int8_t> tail;
  std::optional<llvm::DenseSet<const Type *>> containers;
};

}
