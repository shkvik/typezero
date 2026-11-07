#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/reflect/reflect_checker/reflect_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ReflectChecker {
public:
  explicit ReflectChecker(CheckState &owner);

  Expr *checkReflection(CallExpr *call, MemberExpr *member, const Type *expected);
  void noteReflectedConversion(const Type *from, const Type *to, const CastExpr *cast);
  void noteConstructorFlow(const Type *from, const Type *to);
  void finishReflection();
  bool onlyChecked() const;

private:
  using ResultConstructible = Result<ClassDecl *, ReflectCheckerConstructibleError>;
  using ResultFillableClass = Result<ClassDecl *, ReflectCheckerFillableClassError>;

  static ResultFillableClass fillableClass(ClassDecl *made);
  static void classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out);
  static void constructorsIn(const Type *type, llvm::SmallVectorImpl<const Type *> &out,
                             llvm::SmallPtrSetImpl<const Type *> &seen);
  static bool instanceOf(const Type *made, const Type *of);
  static Expr *stripImplicit(Expr *expr);

  Expr *okOf(Expr *value);
  const Type *reflectError();
  ResultConstructible constructible(ClassDecl *cls, llvm::ArrayRef<const Type *> args);
  void checkReflectNews();

  CheckState &state;
};

}
