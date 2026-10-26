#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ReflectChecker {
public:
  explicit ReflectChecker(Checker &checker);

  Expr *checkReflection(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkFromJsonClass(CallExpr *call, NameExpr *name);
  std::string classJsonProblem(const ClassDecl *cls);
  void noteReflectedConversion(const Type *from, const Type *to, const CastExpr *cast);
  void finishReflection();
  bool onlyChecked() const;

private:
  Checker &checker_;
  llvm::SmallPtrSet<ClassDecl *, 8> escaped_;
  llvm::SmallPtrSet<ClassDecl *, 8> reflectedObjects_;
  bool reflectGetAny_ = false;
  bool jsonReflect_ = false;
  llvm::DenseMap<const ClassDecl *, std::string> jsonProblems_;
};

}
