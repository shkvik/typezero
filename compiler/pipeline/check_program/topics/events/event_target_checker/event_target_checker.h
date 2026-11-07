#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class EventTargetChecker {
public:
  explicit EventTargetChecker(CheckState &owner);

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
  CheckState &state;
};

}
