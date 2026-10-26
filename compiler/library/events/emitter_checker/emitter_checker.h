#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class EmitterChecker {
public:
  explicit EmitterChecker(Checker &checker);

  Expr *checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls, bool super);
  Expr *callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls, llvm::StringRef name,
                          llvm::ArrayRef<const Type *> bound, const std::vector<bool> *given,
                          bool super);
  void reportEmitterMethodValue(MemberExpr *member);
  static std::string libraryExportHint(llvm::StringRef name);
  Expr *checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  Expr *collectEmitterCall(CallExpr *call, MemberExpr *member, const std::string &site);

private:
  Checker &checker_;
};

}
