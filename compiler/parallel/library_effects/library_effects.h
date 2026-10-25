#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// What the calls of the library and of the built-ins read and write.
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
                       bool handled = false);
  void listen(const Expr *f, Channel channel);
  void libraryReads(const Expr *e, llvm::SMLoc at);
  void dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use);
  void libraryState(llvm::StringRef cls, llvm::StringRef name, std::optional<Ref> self, bool write,
                    llvm::SMLoc at);
  bool librarySetter(const MemberExpr *m, const Expr *value);
  void builtinEffects(const CallExpr *c, const Expr *recv);

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
