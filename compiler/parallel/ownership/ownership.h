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
  std::optional<Ref> substituted(Ref r, const FnInfo &callee, FunctionId caller, const CallRec &rec,
                                 const Callee &call);
  void rewrite();
  bool confined(const FunctionDecl *fn) const;
  bool confinedVar(const VarSymbol *v) const;

private:
  using Roots = llvm::SmallVector<const VarSymbol *, 2>;
  using Needs = std::vector<FieldKey>;
  struct Site {
    FunctionId fn = kModuleFunction;
    unsigned rec = 0;
  };
  struct Store {
    const Type *type = nullptr;
    const Expr *value = nullptr;
    FunctionId fn = kModuleFunction;
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
  bool selfOwned(FunctionId fi, const CallRec &rec, const Callee &callee, bool strict);
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
  llvm::DenseSet<const FunctionDecl *> goodClosures_;
  llvm::DenseSet<const FunctionDecl *> badClosures_;
  llvm::DenseMap<const VarSymbol *, std::vector<const FunctionDecl *>> capturers_;
  void leak(const Expr *e);
  void returnParams(const Expr *e);
  bool returnsBack(const CallExpr *c, size_t index);
  llvm::DenseSet<const VarSymbol *> returned_;
  void visitStmt(const Stmt *s);
  void visit(const Expr *e);
  void visitCall(const CallExpr *c);
  void literal(const ObjectLitExpr *o);
  void dirty(const Expr *object, llvm::StringRef field);
  void dirtyAll(const Expr *e);
  bool thisFirst(const CallRec &rec) const;
  bool tailAwaited(FunctionId fi);
  bool handsOver(FunctionId fi, const CallRec &rec, Mode mode, bool strict);
  int argOf(FunctionId fi, const CallRec &rec, const Callee &callee, size_t param, bool strict);
  bool passesOwned(const CallExpr *c, size_t index);
  bool reflectKeepsArgs(const CallExpr *c, bool self);
  bool callersOwned(FunctionId fi, size_t index, bool strict);
  bool poolFresh(const Type *item) const;
  bool callFresh(FunctionId fi, const CallExpr *c) const;
  bool valueOwn(FunctionId fi, const Expr *e, bool params, unsigned depth);
  bool fieldOpen(const FieldKey &key);
  bool fieldOwn(const FieldKey &key);
  bool needsOwn(const Needs &needs);
  bool own(const OwnedRef *owned);
  Ref interned(const VarSymbol *root, const Type *type, Needs needs, bool whole);
  void scan(FunctionId fi);
  void decideOnce();
  struct SelfStore {
    const VarSymbol *self = nullptr;
    const Expr *value = nullptr;
    FunctionId fi = kModuleFunction;
    Needs needs;
  };
  std::vector<SelfStore> selfStores_;
  llvm::DenseSet<const VarSymbol *> forced_;
  Analysis &analysis_;
  const FunctionDecl *fn_ = nullptr;
  FunctionId fi_ = kModuleFunction;
  bool collecting_ = false;
  const Expr *heldLiteral_ = nullptr;
  const VarSymbol *holder_ = nullptr;
  const FunctionDecl *returningIn_ = nullptr;
  llvm::DenseSet<const VarSymbol *> leaked_;
  llvm::DenseSet<const VarSymbol *> leaking_;
  llvm::DenseSet<const VarSymbol *> owned_;
  llvm::DenseMap<const VarSymbol *, std::pair<const FunctionDecl *, unsigned>> paramOf_;
  llvm::DenseMap<const VarSymbol *, FunctionId> freshOf_;
  llvm::DenseMap<const VarSymbol *, const Expr *> loopInit_;
  std::vector<std::vector<Site>> sites_;
  std::vector<std::vector<const Expr *>> returns_;
  std::map<const char *, std::vector<Store>> stores_;
  std::map<const char *, std::vector<const Type *>> dirty_;
  std::map<FieldKey, bool> open_;
  std::set<FieldKey> asked_;
  std::set<FieldKey> evaluated_;
  std::set<FieldKey> notOwn_;
  bool deciding_ = false;
  llvm::DenseSet<FunctionId> fresh_;
  std::vector<int8_t> tail_;
  std::optional<llvm::DenseSet<const Type *>> containers_;
};

}
