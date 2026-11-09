#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class FlowChecker;
class TypeResolver;
class StaticChecker;
class TypeRelations;
class ReflectChecker;
class ReturnInferrer;
class MethodCallChecker;

class ClassValueChecker {
public:
  using ResultClassMemberRead = Result<Expr *, ClassValueCheckerCheckClassRefMemberError>;
  using ResultConstructorArgs = Result<const Type *, ClassValueCheckerConstructorArgsError>;
  using ResultConstructorType = Result<const Type *, ClassValueCheckerResolveConstructorTypeError>;

  ClassValueChecker(SemanticState &owner, SemanticParts &all);

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

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  StaticChecker *staticChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReflectChecker *reflectChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
};

}
