#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class EventMaps;
class FieldFacts;
class CallChecker;
class DictMethods;
class ExprChecker;
class TypeDeclarer;
class ClassDeclarer;
class TypeRelations;
class ProcessChecker;
class ArgumentChecker;
class OverloadResolver;
class ClassMemberChecker;

class ObjectChecker {
public:
  std::string fieldText(const Expr *object, llvm::StringRef field) const;
  using ResultObjectFieldValue = Result<Expr *, ObjectCheckerCheckFieldError>;
  using ResultFieldCallIsTyped = Result<Expr *, ObjectCheckerCheckFieldCallError>;
  using ResultObjectStaticCall = Result<Expr *, ObjectCheckerCheckObjectCallError>;

  ObjectChecker(SemanticState &owner, SemanticParts &all);

  void walkForIn(ForOfStmt *stmt);
  ResultObjectFieldValue checkField(MemberExpr *member);
  Expr *enumObject(NameExpr *name, const Type *expected);
  const Type *checkObjectFieldTarget(MemberExpr *member);
  ResultFieldCallIsTyped checkFieldCall(CallExpr *call, MemberExpr *member);
  std::string objectMismatch(const Type *from, const Type *to, bool conflict = false) const;
  ResultObjectStaticCall checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  std::optional<std::string> chainText(const Expr *expr) const;
  using ResultEnumMemberValues = Result<Expr *, ObjectCheckerEnumMembersError>;
  using ResultForInKeysAllowed = Result<const Type *, ObjectCheckerWalkableError>;
  using ResultLengthIsNotFixed = Result<const Type *, ObjectCheckerFixedLengthError>;
  using ResultTargetFieldFound = Result<const Field *, ObjectCheckerFieldExistsError>;
  using ResultTargetObjectType = Result<const Type *, ObjectCheckerObjectDefinedError>;
  using ResultTargetIsWritable = Result<const Field *, ObjectCheckerFieldWritableError>;

  bool isInherited(llvm::StringRef name) const;
  bool hasMethodFields(const ClassDecl *classDecl) const;
  std::string noFieldText(const Type *type, llvm::StringRef name) const;
  ResultForInKeysAllowed walkable(const Type *type, llvm::StringRef text) const;
  ResultLengthIsNotFixed fixedLength(const Type *type, const MemberExpr *member) const;
  ResultTargetFieldFound fieldExists(const Type *type, const MemberExpr *member) const;
  ResultEnumMemberValues enumMembers(CallExpr *call, EnumDecl *enumDecl,
                                     llvm::StringRef functionName);
  ResultTargetObjectType objectDefined(const Type *type, const MemberExpr *member) const;
  ResultTargetIsWritable fieldWritable(const Field *field, const Type *type,
                                       const MemberExpr *member) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  EventMaps *eventMaps = nullptr;
  FieldFacts *fieldFacts = nullptr;
  CallChecker *callChecker = nullptr;
  DictMethods *dictMethods = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
};

}
