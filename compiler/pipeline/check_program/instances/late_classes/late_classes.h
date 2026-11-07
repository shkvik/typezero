#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class LateClasses {
public:
  explicit LateClasses(CheckState &owner);

  void checkLateClasses();
  void makeLateClass(ClassDecl *cls);
  void checkStaticFields(ClassDecl *cls);
  void markLateOverrides(ClassDecl *cls);
  bool isTemplate(const FuncSymbol *generic);
  bool hasClassExpr(const FunctionDecl *fn);
  void markLateOverrides(SourceFile &program);
  void bindOuterTypeParams(const ClassDecl *cls);
  void checkTemplateInstance(InstanceRecord *instance);
  Expr *checkClassExpr(ClassExpr *expr, const Type *expected);
  void replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc);

private:
  using ResultBaseClassUsable = Result<ClassDecl *, LateClassesBaseClassError>;
  using ResultStaticTypeKnown = Result<const Type *, LateClassesStaticTypeError>;
  using ResultNoOldObjectHeld = Result<const ClassField *, LateClassesOldObjectError>;
  using ResultFieldNameIsFree = Result<const ClassField *, LateClassesReplacedFieldError>;

  static ResultStaticTypeKnown staticType(const ClassField &field, const Type *type);
  static ResultNoOldObjectHeld oldObject(const ClassDecl *cls, const ClassField &field);
  static bool holdsType(const Type *type, const Type *target,
                        llvm::SmallPtrSetImpl<const Type *> &seen);
  static ResultFieldNameIsFree replacedField(const ClassDecl *cls, const ClassDecl *replacement,
                                             const ClassDecl *child, const ClassField &field);

  void relink(ClassDecl *cls);
  void noteMethodNames(const ClassDecl *cls);
  ResultBaseClassUsable baseClass(const Type *type) const;

  CheckState &state;
};

}
