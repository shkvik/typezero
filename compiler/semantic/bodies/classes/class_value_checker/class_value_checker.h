#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassLinks;
class SemanticParts;
class ExprChecker;
class FlowChecker;
class TypeResolver;
class StaticChecker;
class TypeRelations;
class ClassDeclarer;
class ReflectChecker;
class MethodCallChecker;

class ClassValueChecker {
public:
  using ResultClassMemberRead = Result<Expr *, ClassValueCheckerCheckClassRefMemberError>;
  using ResultConstructorArgs = Result<const Type *, ClassValueCheckerConstructorArgsError>;
  using ResultConstructorType = Result<const Type *, ClassValueCheckerResolveConstructorTypeError>;

  ClassValueChecker(SemanticState &owner, SemanticParts &all);

  ResultConstructorArgs constructorArgs();
  const Type *classValueType(ClassDecl *classDecl);
  ClassDecl *finalClass(ClassDecl *classDecl) const;
  Expr *classValueAt(ClassDecl *classDecl, llvm::SMLoc loc);
  Expr *checkClassValue(NameExpr *name, ClassDecl *classDecl);
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
  ResultClassHasItsType typedClass(ClassDecl *classDecl) const;
  ResultArgumentNotVoid argumentValue(const Expr *arg) const;
  ResultNewOfClassValue valueNewClass(const Type *type) const;
  ResultClassIsDeclared classDeclared(const ClassDecl *classDecl) const;
  ResultNoTypeArguments runtimeTypeArgs(const NewExpr *expr) const;

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  StaticChecker *staticChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ReflectChecker *reflectChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
};

}
