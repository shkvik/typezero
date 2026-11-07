#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ReflectCallChecker {
public:
  explicit ReflectCallChecker(CheckState &owner);

  Expr *checkReflectArgs(Expr *args);
  FunctionDecl *knownReflectMethod(ClassDecl *cls, llvm::StringRef name, bool &several);
  const FunctionDecl *heldReflectMethod(const Expr *f);
  Expr *directReflectApply(CallExpr *call, const FunctionDecl *fn);
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  static bool reflectAccepts(const Type *arg, const Type *param);

private:
  static const Type *designType(const Expr *key);
  static bool isDesignKey(const Expr *key);
  static Expr *stripImplicit(Expr *expr);

  CheckState &state;
};

}
