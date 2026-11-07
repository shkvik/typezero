#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <optional>

namespace tz::parallel {

class LibraryEffects {
public:
  explicit LibraryEffects(Analysis &analysis);

  bool inLibrary(llvm::SMLoc loc) const;
  const FunctionDecl *libraryTarget(const CallExpr *c) const;
  void libraryCall(const CallExpr *c, const FunctionDecl *decl, const Use &use);
  void libraryNew(const NewExpr *n);
  void libraryArg(const CallExpr *c, llvm::StringRef cls, llvm::StringRef name, const Expr *arg,
                  llvm::SMLoc at);
  static const Type *poolItemOf(const Type *fn);
  void poolCreate(const Expr *f);
  void libraryCallback(const CallExpr *c, const Expr *f, CallKind kind, bool interval = false,
                       bool handled = false, unsigned ownArgs = 0, const Type *poolItem = nullptr);
  void listen(const Expr *f, Channel channel);
  void libraryReads(const Expr *e, llvm::SMLoc at);
  void dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use);
  void libraryState(llvm::StringRef cls, llvm::StringRef name, std::optional<Ref> self, bool write,
                    llvm::SMLoc at);
  bool readsOnly(const FunctionDecl *decl);

private:
  static bool isEmitterClass(llvm::StringRef cls);
  static bool isTargetClass(llvm::StringRef cls);
  static bool isPoolClass(llvm::StringRef cls);
  static bool isLockedClass(llvm::StringRef cls);
  static bool isContextClass(llvm::StringRef cls);
  static bool readOnlyExpr(const Expr *e);
  static bool readOnlyStmt(const Stmt *s);
  Analysis &analysis_;
  llvm::DenseMap<const FunctionDecl *, bool> readsOnly_;
};

}
