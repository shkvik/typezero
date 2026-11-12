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

  Expr *checkFieldMember(MemberExpr *member, const ClassField *field, const ClassDecl *owner,
                         bool viaThis);
  Expr *checkMethodMember(MemberExpr *member, ClassDecl *classDecl, const Type *expected,
                          bool viaThis);
  const char *accessWord(Access access) const;
  bool sameClass(const ClassDecl *a, const ClassDecl *b) const;
  ResultStoredFieldValue statsDate(ClassDecl *classDecl, MemberExpr *member);
  ResultObjectReadyToUse objectReady(bool viaThis, llvm::StringRef name) const;
  ResultPlainMethodValue methodValue(const FuncSymbol *function, llvm::StringRef name) const;
  ResultFieldTypeIsKnown fieldTyped(const ClassField *field, llvm::StringRef name) const;
  ResultMethodsOfObjects instanceMethods(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultFieldIsSetBefore fieldSet(const ClassDecl *owner, llvm::StringRef name, bool viaThis) const;
  ResultFieldNotReadOnly fieldWritable(const ClassField *field, const ClassDecl *owner,
                                       bool viaThis, const Type *type) const;
  ResultAssignableMember assignableField(const ClassField *field, const ClassDecl *classDecl,
                                         llvm::StringRef name, const Type *type) const;

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
