#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ReflectCallChecker {
public:
  explicit ReflectCallChecker(SemanticState &owner);

  Expr *checkReflectArgs(Expr *args);
  FunctionDecl *knownReflectMethod(ClassDecl *cls, llvm::StringRef name, bool &several);
  const FunctionDecl *heldReflectMethod(const Expr *f);
  Expr *directReflectApply(CallExpr *call, const FunctionDecl *fn);
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  static bool reflectAccepts(const Type *arg, const Type *param);

private:
  using ResultKeyOfRecords = Result<const Type *, ReflectCallCheckerKeyOfRecordsError>;
  using ResultTargetObject = Result<const Type *, ReflectCallCheckerTargetObjectError>;
  using ResultArgumentList = Result<const Type *, ReflectCallCheckerArgumentListError>;

  static ResultKeyOfRecords keyOfRecords(const Type *type);
  static ResultTargetObject targetObject(const Type *type);
  static ResultArgumentList argumentList(const Type *type);
  static const Type *designType(const Expr *key);
  static bool isDesignKey(const Expr *key);
  static Expr *stripImplicit(Expr *expr);

  SemanticState &state;
};

}
