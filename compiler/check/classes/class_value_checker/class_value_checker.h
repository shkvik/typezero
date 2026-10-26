#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClassValueChecker {
public:
  explicit ClassValueChecker(Checker &checker);

  Expr *checkClassRefMember(MemberExpr *member);
  static const Type *constructedBy(const Type *classValue);
  const Type *classValueType(ClassDecl *cls);
  static ClassDecl *finalClass(ClassDecl *cls);
  const Type *resolveConstructorType(const TypeRef &ref);
  const Type *constructorArgs(llvm::SMLoc loc);
  Expr *checkValueNew(NewExpr *expr, const Type *expected);
  Expr *checkClassValue(NameExpr *name, ClassDecl *cls);
  Expr *classValueAt(ClassDecl *cls, llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
