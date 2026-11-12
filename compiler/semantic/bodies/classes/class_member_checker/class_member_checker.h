#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker_errors.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker_types.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class ClassLinks;
class ClassBases;
class SemanticParts;
class EventMaps;
class FieldFacts;
class FsDeclarer;
class ClassChecker;
class LibraryCalls;
class ClassDeclarer;
class ObjectChecker;
class TypeRelations;
class EmitterChecker;
class ReturnInferrer;
class OverloadResolver;
class TzModulesChecker;

class ClassMemberChecker {
public:
  using ResultMemberAccessible = Result<const ClassDecl *, ClassMemberCheckerCheckAccessError>;

  ClassMemberChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkClassMember(MemberExpr *member, const Type *expected);
  const Type *checkFieldTarget(MemberExpr *member, AssignForm form = AssignForm::PLAIN);
  ResultMemberAccessible checkAccess(const ClassDecl *classDecl, Access access,
                                     llvm::StringRef name, const Expr *receiver = nullptr);

private:
  using ResultFieldTypeIsKnown = Result<const Type *, ClassMemberCheckerFieldTypedError>;
  using ResultFieldIsSetBefore = Result<llvm::StringRef, ClassMemberCheckerFieldSetError>;
  using ResultObjectReadyToUse = Result<llvm::StringRef, ClassMemberCheckerObjectReadyError>;
  using ResultStoredFieldValue = Result<const ClassDecl *, ClassMemberCheckerStatsDateError>;
  using ResultPlainMethodValue = Result<const FuncSymbol *, ClassMemberCheckerMethodValueError>;
  using ResultFieldNotReadOnly = Result<const ClassField *, ClassMemberCheckerFieldWritableError>;
  using ResultAssignableMember = Result<const ClassField *, ClassMemberCheckerAssignableFieldError>;
  using ResultMethodsOfObjects =
      Result<std::vector<FunctionDecl *>, ClassMemberCheckerInstanceMethodsError>;

  bool isSameClass(const ClassDecl *first, const ClassDecl *second) const;
  bool isDerivedFrom(const ClassDecl *from, const ClassDecl *base) const;
  ResultMemberAccessible checkProtectedReceiver(const ClassDecl *classDecl, llvm::StringRef name,
                                                const Expr *receiver) const;
  ResultFieldIsSetBefore checkFieldSet(const ClassDecl *owner, llvm::StringRef name,
                                       bool isViaThis) const;
  ResultFieldTypeIsKnown checkFieldTyped(const ClassField *field, llvm::StringRef name) const;
  ResultMethodsOfObjects checkInstanceMethods(const ClassDecl *classDecl,
                                              llvm::StringRef name) const;
  bool hasStaticField(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultObjectReadyToUse checkObjectReady(bool isViaThis, llvm::StringRef name) const;
  ResultPlainMethodValue checkMethodValue(const FuncSymbol *function, llvm::StringRef name) const;
  Expr *checkFieldMember(MemberExpr *member, const ClassField *field, const ClassDecl *owner,
                         bool isViaThis);
  Expr *checkMethodMember(MemberExpr *member, ClassDecl *classDecl, const Type *expected,
                          bool isViaThis);
  MethodMember methodForm(ClassDecl *classDecl, FuncSymbol *function) const;
  ResultStoredFieldValue checkStatsDate(ClassDecl *classDecl, MemberExpr *member);
  ResultAssignableMember checkAssignableField(const ClassField *field, const ClassDecl *classDecl,
                                              llvm::StringRef name, const Type *type) const;
  ResultFieldNotReadOnly checkFieldWritable(const ClassField *field, const ClassDecl *owner,
                                            bool isViaThis, const Type *type) const;
  bool reportUnsetField(const MemberExpr *member, const ClassDecl *owner, bool isViaThis);

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  FieldFacts *fieldFacts = nullptr;
  FsDeclarer *fsDeclarer = nullptr;
  ClassLinks *classLinks = nullptr;
  ClassBases *classBases = nullptr;
  ClassChecker *classChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ObjectChecker *objectChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
};

}
