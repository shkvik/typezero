#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/infer/late_classes/late_classes_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class PlacedNodes;
class ClassBases;
class ClassLinks;
class FsDeclarer;
class TypeParams;
class ExprChecker;
class ClassChecker;
class ClassDeclarer;
class TypeRelations;
class ModuleDeclarer;
class ReflectChecker;
class ReturnInferrer;
class FunctionChecker;
class DecoratorChecker;
class ClassValueChecker;
class GenericInstantiator;

class LateClasses {
public:
  LateClasses(SemanticState &owner, SemanticParts &all);

  void walkLateClasses();
  void rememberEmittedLateClasses();
  void makeLateClass(ClassDecl *classDecl);
  void walkStaticFields(ClassDecl *classDecl);
  bool isTemplate(const FuncSymbol *generic);
  bool hasClassExpr(const FunctionDecl *functionDecl);
  void markLateOverrides(SourceFile &program);
  void bindOuterTypeParams(const ClassDecl *classDecl);
  void walkTemplateInstance(InstanceRecord *instance);
  Expr *checkClassExpr(ClassExpr *expr);
  void replaceClass(ClassDecl *classDecl, ClassDecl *replacement, llvm::SMLoc loc);

private:
  using ResultBaseClassUsable = Result<ClassDecl *, LateClassesBaseClassError>;
  using ResultStaticTypeKnown = Result<const Type *, LateClassesStaticTypeError>;
  using ResultNoOldObjectHeld = Result<const ClassField *, LateClassesOldObjectError>;
  using ResultFieldNameIsFree = Result<const ClassField *, LateClassesReplacedFieldError>;

  bool hasClassExprIn(const PlacedNodes &nodes) const;
  ResultStaticTypeKnown staticType(const ClassField &field, const Type *type) const;
  ResultNoOldObjectHeld oldObject(const ClassDecl *classDecl, const ClassField &field) const;
  bool isHoldingType(const Type *type, const Type *target) const;
  ResultFieldNameIsFree replacedField(const ClassDecl *classDecl, const ClassDecl *replacement,
                                      const ClassDecl *child, const ClassField &field) const;

  void registerLateClass(ClassDecl *classDecl);
  void rememberLateScope(ClassDecl *classDecl);
  void linkLateBase(ClassDecl *classDecl);
  void nameAnonymousClass(ClassDecl *classDecl) const;
  void declareLateMembers(ClassDecl *classDecl);
  void rememberMethodModules(const ClassDecl *classDecl);
  void relink(ClassDecl *classDecl);
  void relinkClass(ClassDecl *classDecl);
  void noteMethodNames(const ClassDecl *classDecl);
  ResultBaseClassUsable baseClass(const Type *type) const;
  void walkStaticField(ClassField &field);
  void walkLateClass(ClassDecl *classDecl);
  void reportLateError(const LateError &error);
  void markClassOverrides(ClassDecl *classDecl);
  void walkClassDecorators(ClassDecl *classDecl);
  const FunctionDecl *innerGenericCopy() const;
  bool isExtending(const ClassDecl *classDecl) const;
  const Type *declaredStaticType(const ClassField &field) const;
  void attachLateParent(ClassDecl *classDecl, ClassDecl *parent);
  void swapTypes(ClassDecl *classDecl, ClassDecl *replacement);
  void moveChild(ClassDecl *classDecl, ClassDecl *replacement, ClassDecl *child);
  std::vector<ClassDecl *> declaredChildren(const ClassDecl *classDecl,
                                            const ClassDecl *replacement);

  SemanticState &state;
  ClassBases *classBases = nullptr;
  ClassLinks *classLinks = nullptr;
  FsDeclarer *fsDeclarer = nullptr;
  TypeParams *typeParams = nullptr;
  ExprChecker *exprChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ModuleDeclarer *moduleDeclarer = nullptr;
  ReflectChecker *reflectChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  DecoratorChecker *decoratorChecker = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
};

}
