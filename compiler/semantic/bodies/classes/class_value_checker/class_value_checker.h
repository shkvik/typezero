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

  ResultConstructorArgs checkConstructorArgs();
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

  const Type *classRefType(const ClassDecl *classDecl) const;
  Expr *afterClassValue(Expr *object, Expr *result);
  ResultNewOfClassValue checkValueNewClass(const Type *type) const;
  ResultClassHasItsType checkTypedClass(ClassDecl *classDecl) const;
  ResultNoTypeArguments checkRuntimeTypeArgs(const NewExpr *expr) const;
  ResultArgumentNotVoid checkArgumentValue(const Expr *arg) const;
  Expr *failValueNew(NewExpr *expr);
  Expr *checkClassValueNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected);
  std::vector<const Type *> reflectedArgTypes(NewExpr *expr);
  ResultClassIsDeclared checkClassDeclared(const ClassDecl *classDecl) const;
  bool hasOwnNameMethod(const ClassDecl *classDecl) const;
  Expr *classNameText(MemberExpr *member, const ClassDecl *classDecl);

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
