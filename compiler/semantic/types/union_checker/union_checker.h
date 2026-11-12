#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/union_checker/union_checker_errors.h"

namespace tz {

class ClassLinks;
class ClassBases;
class SemanticParts;
class FieldFacts;
class BytesChecker;
class ClassDeclarer;
class ObjectChecker;
class ReturnInferrer;
class ArgumentChecker;
class ClassMemberChecker;

class UnionChecker {
public:
  using ResultUnionMemberTyped = Result<Expr *, UnionCheckerCheckUnionMemberError>;
  using ResultUnionMethodTyped = Result<Expr *, UnionCheckerCheckUnionMethodCallError>;
  using ResultUnionFieldTarget = Result<const Type *, UnionCheckerCheckUnionFieldTargetError>;

  UnionChecker(SemanticState &owner, SemanticParts &all);

  ResultUnionMemberTyped checkUnionMember(MemberExpr *member);
  ResultUnionFieldTarget checkUnionFieldTarget(MemberExpr *member);
  const Type *commonBaseWith(const Type *type, llvm::StringRef name) const;
  ResultUnionMethodTyped checkUnionMethodCall(CallExpr *call, MemberExpr *member);
  std::optional<std::vector<const Type *>> unhandledCases(const SwitchStmt *stmt) const;

private:
  struct TagHint {
    std::string text;
    bool isUnique;
  };

  struct UnionMethodPart {
    const Type *type;
    UnionMethod entry;
  };

  struct UnionMethods {
    const Type *type;
    std::vector<UnionMethod> methods;
  };

  using ResultMemberAllowed = Result<void, UnionCheckerCheckUnionMemberError>;
  using ResultMethodAllowed = Result<void, UnionCheckerCheckUnionMethodCallError>;
  using ResultTargetAllowed = Result<void, UnionCheckerCheckUnionFieldTargetError>;

  using ResultMemberFieldType = Result<const Type *, UnionCheckerCheckUnionMemberError>;
  using ResultUnionMethodList = Result<UnionMethods, UnionCheckerCheckUnionMethodCallError>;
  using ResultUnionMethodPart = Result<UnionMethodPart, UnionCheckerCheckUnionMethodCallError>;
  using ResultMemberFieldList =
      Result<std::vector<const Type *>, UnionCheckerCheckUnionMemberError>;

  ResultMemberAllowed checkNotTagged(const MemberExpr *member, const Type *type) const;
  ResultMemberAllowed checkFieldKnown(const MemberExpr *member, const Type *type) const;
  ResultMemberAllowed checkFieldAccessible(const MemberExpr *member, const ClassDecl *classDecl);
  ResultTargetAllowed checkTargetAccessible(const MemberExpr *member, const ClassDecl *classDecl);
  ResultMethodAllowed checkNarrowedObjects(const MemberExpr *member, const Type *receiver) const;
  ResultMemberAllowed checkJoinable(const MemberExpr *member, const Type *type,
                                    const Type *joined) const;
  ResultMethodAllowed checkSameMethodType(const MemberExpr *member, const Type *common,
                                          const Type *partType) const;
  ResultMethodAllowed checkCallablePart(const MemberExpr *member, const Type *memberType,
                                        const Type *partType) const;
  ResultTargetAllowed checkSameTarget(const MemberExpr *member, const Type *type,
                                      const Type *common, const Type *fieldType) const;

  ResultMemberFieldList memberFieldTypes(const MemberExpr *member, const Type *type);
  ResultUnionMethodPart memberMethod(const MemberExpr *member, const Type *memberType);
  ResultUnionMethodList resolveUnionMethods(const MemberExpr *member, const Type *receiver);
  ResultMemberFieldType memberFieldType(const MemberExpr *member, const Type *type,
                                        const Type *memberType);
  ResultUnionFieldTarget memberTargetType(const MemberExpr *member, const Type *type,
                                          const Type *memberType);
  ResultUnionMethodPart classFieldPart(const MemberExpr *member, const Type *memberType,
                                       const ClassDecl *classDecl);
  ResultUnionMethodPart classMemberMethod(const MemberExpr *member, const Type *memberType,
                                          ClassDecl *classDecl);
  ResultUnionMethodPart classVariantMethod(const MemberExpr *member, const Type *memberType,
                                           ClassDecl *classDecl, FunctionDecl *variant);

  bool isAllBytes(const Type *type) const;
  bool isLiteralSet(const Type *type) const;
  bool isAllObjects(const Type *type) const;
  const Type *caseLiteral(const Expr *test) const;
  ClassDecl *completedClassOf(const Type *memberType);
  UnionMethodPart missingPart(const Type *memberType) const;
  std::vector<const Type *> baseChain(const Type *type) const;
  bool isTagField(const Type *type, llvm::StringRef tagName) const;
  bool hasMember(const Type *candidate, llvm::StringRef name) const;
  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  bool isHoldingLiteral(const Type *set, const Type *literal) const;
  bool hasDataField(const ClassDecl *classDecl, llvm::StringRef name) const;
  std::string holderHint(const MemberExpr *member, const Type *type) const;
  const Type *lastHolder(const MemberExpr *member, const Type *type) const;
  std::string lackingHint(const MemberExpr *member, const Type *type) const;
  UnionMethodPart fieldPart(const Type *memberType, llvm::StringRef name) const;
  bool isUnknownToPlainObjects(const MemberExpr *member, const Type *type) const;
  bool isCommonBase(const Type *candidate, llvm::ArrayRef<const Type *> members) const;
  UnionMethod variantEntry(const Type *memberType, ClassDecl *classDecl,
                           FuncSymbol *function) const;
  const Type *taggedHolder(const MemberExpr *member, const Type *type,
                           llvm::StringRef tagName) const;
  const Field *assignableField(const MemberExpr *member, const Type *memberType,
                               const ClassDecl *classDecl) const;
  const Field *unionMemberField(const MemberExpr *member, const Type *memberType,
                                const ClassDecl *classDecl) const;
  bool isTagUnique(const MemberExpr *member, const Type *type, llvm::StringRef tagName,
                   const Type *tag) const;
  std::optional<TagHint> tagHint(const MemberExpr *member, const Type *type,
                                 llvm::StringRef tagName) const;

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  ClassLinks *classLinks = nullptr;
  ClassBases *classBases = nullptr;
  BytesChecker *bytesChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
};

}
