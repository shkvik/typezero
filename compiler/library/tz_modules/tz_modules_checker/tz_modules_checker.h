#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TzModulesChecker {
public:
  explicit TzModulesChecker(Checker &checker);

  Expr *adaptResult(Expr *expr, const Type *to);
  void checkExternal(FunctionDecl *fn);
  static FunctionDecl *accessorOf(ClassDecl *cls, llvm::StringRef name,
                                  FunctionDecl::Accessor kind);
  Expr *checkAccessorRead(MemberExpr *member, ClassDecl *cls, const Type *expected);
  const Type *checkAccessorTarget(MemberExpr *member, ClassDecl *cls, bool compound);
  Expr *checkSetterCall(AssignExpr *assign);
  bool rejectAccessorCall(CallExpr *call, MemberExpr *member, FuncSymbol *sym);
  void noteHttpsCall(CallExpr *call, const FunctionDecl *fn);
  void noteHttpsValue(const FunctionDecl *fn, llvm::SMLoc loc);
  std::string knownPrefix(const Expr *expr) const;

private:
  Checker &checker_;
  llvm::SmallPtrSet<const CallExpr *, 8> accessorCalls_;
};

}
