#pragma once

#include "compiler/parallel/analysis/analysis.h"
#include "llvm/ADT/DenseMap.h"

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
  void libraryCallback(const CallExpr *c, const Expr *f, CallKind kind, bool interval = false,
                       bool handled = false, bool served = false);
  void listen(const Expr *f, Channel channel);
  void libraryReads(const Expr *e, llvm::SMLoc at);
  void dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use);
  void libraryState(llvm::StringRef cls, llvm::StringRef name, std::optional<Ref> self, bool write,
                    llvm::SMLoc at);
  bool readsOnly(const FunctionDecl *decl);

private:
  Analysis &analysis_;
  llvm::DenseMap<const FunctionDecl *, bool> readsOnly_;
};

}
