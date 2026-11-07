#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

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
  static void classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out);
  static void constructorsIn(const Type *type, llvm::SmallVectorImpl<const Type *> &out,
                             llvm::SmallPtrSetImpl<const Type *> &seen);
  static bool instanceOf(const Type *made, const Type *of);
  static Expr *stripImplicit(Expr *expr);

  Expr *okOf(Expr *value);
  const Type *reflectError();
  std::string constructProblem(ClassDecl *cls, llvm::ArrayRef<const Type *> args);
  void checkReflectNews();

  CheckState &state;
};

}
