#pragma once

#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/dict_methods/dict_methods_errors.h"
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
  std::string objectMismatch(const Type *from, const Type *to, bool isConflict) const;
  ResultObjectStaticCall checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultEnumMemberValues = Result<Expr *, ObjectCheckerEnumMembersError>;
  using ResultForInKeysAllowed = Result<const Type *, ObjectCheckerWalkableError>;
  using ResultLengthIsNotFixed = Result<const Type *, ObjectCheckerFixedLengthError>;
  using ResultTargetFieldFound = Result<const Field *, ObjectCheckerFieldExistsError>;
  using ResultTargetObjectType = Result<const Type *, ObjectCheckerObjectDefinedError>;
  using ResultTargetIsWritable = Result<const Field *, ObjectCheckerFieldWritableError>;
  using ResultListedType = Result<const Type *, ObjectCheckerCheckObjectCallError>;

  struct OwnedField {
    const Type *owner;
    const Field *field;
  };

  std::optional<std::string> chainText(const Expr *expr) const;
  bool isIdentifierChar(char letter) const;
  bool isInherited(llvm::StringRef name) const;
  std::string noFieldText(const Type *type, llvm::StringRef name) const;
  ResultObjectStaticCall checkObjectGroupBy(CallExpr *call);
  ObjectCheckerCheckObjectCallErrorCode groupByCode(DictMethodsCheckGroupByErrorCode code) const;
  ResultObjectStaticCall checkObjectFromEntries(CallExpr *call, const Type *expected);
  ObjectCheckerCheckObjectCallErrorCode
  fromEntriesCode(DictMethodsCheckFromEntriesErrorCode code) const;
  ResultObjectStaticCall checkObjectListing(CallExpr *call, GlobalMember listing,
                                            llvm::StringRef functionName);
  Expr *typedListing(CallExpr *call, GlobalMember listing, const Type *value) const;
  ResultObjectStaticCall checkDictListing(CallExpr *call, llvm::StringRef functionName);
  ResultListedType checkListedClass(const Type *type, llvm::StringRef functionName);
  ResultListedType checkListedElement(const Type *type, llvm::StringRef functionName);
  std::vector<OwnedField> listedFields(const Type *type);
  void addSubtypeFields(const Type *subtype, std::vector<OwnedField> &fields);
  void addOwnedFields(const Type *owner, std::vector<OwnedField> &fields) const;
  ResultObjectStaticCall checkEnumListing(CallExpr *call, EnumDecl *enumDecl, GlobalMember listing,
                                          llvm::StringRef functionName);
  ResultEnumMemberValues checkEnumMembers(CallExpr *call, EnumDecl *enumDecl, GlobalMember listing,
                                          llvm::StringRef functionName);
  Expr *enumListElement(const CallExpr *call, const EnumDecl *enumDecl,
                        const EnumMember &enumMember, GlobalMember listing);
  ResultForInKeysAllowed checkWalkable(const Type *type, llvm::StringRef text) const;
  std::string indexedKindText(const Type *type) const;
  void addKeyGuard(ForOfStmt *stmt, const NameExpr *variable);
  Expr *recordAgain(llvm::SMLoc loc, const NameExpr *variable);
  BlockStmt *bodyBlock(ForOfStmt *stmt);
  std::string forInText(const Expr *object, const Type *type) const;
  Expr *forInKeys(Expr *object, bool isWalking);
  ResultFieldCallIsTyped checkFieldValueCall(CallExpr *call, const Type *type);
  ResultTargetObjectType checkObjectDefined(const Type *type, const MemberExpr *member) const;
  ResultLengthIsNotFixed checkFixedLength(const Type *type, const MemberExpr *member) const;
  ResultTargetFieldFound checkFieldExists(const Type *type, const MemberExpr *member) const;
  ResultTargetIsWritable checkFieldWritable(const Field *field, const Type *type,
                                            const MemberExpr *member) const;
  std::string fieldTypeMismatch(const Field &field, const Field &target, const Type *to,
                                bool isConflict) const;

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
