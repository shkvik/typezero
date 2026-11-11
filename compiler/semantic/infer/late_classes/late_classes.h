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
  void makeLateClass(ClassDecl *cls);
  void walkStaticFields(ClassDecl *cls);
  void markLateOverrides(ClassDecl *cls);
  bool isTemplate(const FuncSymbol *generic);
  bool hasClassExpr(const FunctionDecl *fn);
  void markLateOverrides(SourceFile &program);
  void bindOuterTypeParams(const ClassDecl *cls);
  void walkTemplateInstance(InstanceRecord *instance);
  Expr *checkClassExpr(ClassExpr *expr, const Type *expected);
  void replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc);

private:
  using ResultBaseClassUsable = Result<ClassDecl *, LateClassesBaseClassError>;
  using ResultStaticTypeKnown = Result<const Type *, LateClassesStaticTypeError>;
  using ResultNoOldObjectHeld = Result<const ClassField *, LateClassesOldObjectError>;
  using ResultFieldNameIsFree = Result<const ClassField *, LateClassesReplacedFieldError>;

  static bool hasClassExprIn(const PlacedNodes &nodes);
  static ResultStaticTypeKnown staticType(const ClassField &field, const Type *type);
  static ResultNoOldObjectHeld oldObject(const ClassDecl *cls, const ClassField &field);
  static bool holdsType(const Type *type, const Type *target);
  static ResultFieldNameIsFree replacedField(const ClassDecl *cls, const ClassDecl *replacement,
                                             const ClassDecl *child, const ClassField &field);

  void registerLateClass(ClassDecl *cls);
  void rememberLateScope(ClassDecl *cls);
  void linkLateBase(ClassDecl *cls);
  static void nameAnonymousClass(ClassDecl *cls);
  void declareLateMembers(ClassDecl *cls);
  void rememberMethodModules(const ClassDecl *cls);
  void relink(ClassDecl *cls);
  void relinkClass(ClassDecl *cls);
  void noteMethodNames(const ClassDecl *cls);
  ResultBaseClassUsable baseClass(const Type *type) const;

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
