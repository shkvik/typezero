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
  static std::string fieldText(const Expr *object, llvm::StringRef field);
  using ResultObjectFieldValue = Result<Expr *, ObjectCheckerCheckFieldError>;
  using ResultFieldCallIsTyped = Result<Expr *, ObjectCheckerCheckFieldCallError>;
  using ResultObjectStaticCall = Result<Expr *, ObjectCheckerCheckObjectCallError>;

  ObjectChecker(SemanticState &owner, SemanticParts &all);

  void checkForIn(ForOfStmt *stmt);
  ResultObjectFieldValue checkField(MemberExpr *member);
  Expr *enumObject(NameExpr *name, const Type *expected);
  const Type *checkObjectFieldTarget(MemberExpr *member);
  ResultFieldCallIsTyped checkFieldCall(CallExpr *call, MemberExpr *member);
  std::string objectMismatch(const Type *from, const Type *to, bool conflict = false) const;
  ResultObjectStaticCall checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static std::string chainText(const Expr *expr);
  using ResultEnumMemberValues = Result<Expr *, ObjectCheckerEnumMembersError>;
  using ResultForInKeysAllowed = Result<const Type *, ObjectCheckerWalkableError>;
  using ResultLengthIsNotFixed = Result<const Type *, ObjectCheckerFixedLengthError>;
  using ResultTargetFieldFound = Result<const Field *, ObjectCheckerFieldExistsError>;
  using ResultTargetObjectType = Result<const Type *, ObjectCheckerObjectDefinedError>;
  using ResultTargetIsWritable = Result<const Field *, ObjectCheckerFieldWritableError>;

  static bool isInherited(llvm::StringRef name);
  static std::u16string utf16(llvm::StringRef text);
  static bool hasMethodFields(const ClassDecl *cls);
  static std::string noFieldText(const Type *type, llvm::StringRef name);
  static ResultForInKeysAllowed walkable(const Type *type, llvm::StringRef text);
  static ResultLengthIsNotFixed fixedLength(const Type *type, const MemberExpr *member);
  static ResultTargetFieldFound fieldExists(const Type *type, const MemberExpr *member);
  ResultEnumMemberValues enumMembers(CallExpr *call, EnumDecl *decl, llvm::StringRef fn);
  static ResultTargetObjectType objectDefined(const Type *type, const MemberExpr *member);
  static ResultTargetIsWritable fieldWritable(const Field *field, const Type *type,
                                              const MemberExpr *member);

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
