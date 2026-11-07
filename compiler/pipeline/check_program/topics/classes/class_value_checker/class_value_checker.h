#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/classes/class_value_checker/class_value_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ClassValueChecker {
public:
  using ResultClassMemberRead = Result<Expr *, ClassValueCheckerCheckClassRefMemberError>;
  using ResultConstructorArgs = Result<const Type *, ClassValueCheckerConstructorArgsError>;
  using ResultConstructorType = Result<const Type *, ClassValueCheckerResolveConstructorTypeError>;

  explicit ClassValueChecker(CheckState &owner);

  ResultConstructorArgs constructorArgs();
  const Type *classValueType(ClassDecl *cls);
  static ClassDecl *finalClass(ClassDecl *cls);
  Expr *classValueAt(ClassDecl *cls, llvm::SMLoc loc);
  Expr *checkClassValue(NameExpr *name, ClassDecl *cls);
  Expr *checkValueNew(NewExpr *expr, const Type *expected);
  static const Type *constructedBy(const Type *classValue);
  ResultClassMemberRead checkClassRefMember(MemberExpr *member);
  ResultConstructorType resolveConstructorType(const TypeRef &ref);

private:
  using ResultClassHasItsType = Result<ClassDecl *, ClassValueCheckerTypedClassError>;
  using ResultNewOfClassValue = Result<const Type *, ClassValueCheckerValueNewClassError>;
  using ResultArgumentNotVoid = Result<const Expr *, ClassValueCheckerArgumentValueError>;
  using ResultClassIsDeclared = Result<const ClassDecl *, ClassValueCheckerClassDeclaredError>;
  using ResultNoTypeArguments = Result<const NewExpr *, ClassValueCheckerRuntimeTypeArgsError>;

  Expr *afterClassValue(Expr *object, Expr *result);
  static ResultClassHasItsType typedClass(ClassDecl *cls);
  static ResultArgumentNotVoid argumentValue(const Expr *arg);
  static ResultNewOfClassValue valueNewClass(const Type *type);
  ResultClassIsDeclared classDeclared(const ClassDecl *cls) const;
  static ResultNoTypeArguments runtimeTypeArgs(const NewExpr *expr);

  CheckState &state;
};

}
