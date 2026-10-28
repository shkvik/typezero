#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class EventTargetChecker {
public:
  explicit EventTargetChecker(Checker &checker);

  Expr *onabortAssign(AssignExpr *assign, MemberExpr *member, bool logical);
  Expr *checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func);
  Expr *callLibraryFunction(CallExpr *call, llvm::StringRef name,
                            llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                            llvm::SMLoc loc);
  Expr *callLibraryStatic(CallExpr *call, ClassDecl *cls, llvm::StringRef name,
                          llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                          llvm::SMLoc loc);

private:
  Expr *callLibrarySymbol(CallExpr *call, FuncSymbol *sym, llvm::ArrayRef<const Type *> bound,
                          const std::vector<bool> &given, llvm::SMLoc loc);
  Checker &checker_;
};

}
