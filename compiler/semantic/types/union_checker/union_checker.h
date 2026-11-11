#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/union_checker/union_checker_errors.h"

namespace tz {

class SemanticParts;
class FieldFacts;
class BytesChecker;
class ClassDeclarer;
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
  static bool isLiteralSet(const Type *type);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);
  static std::string lackingHint(const MemberExpr *member, const Type *type);
  static bool isAllBytes(const Type *type);
  std::optional<std::string> accessProblem(const MemberExpr *member, const ClassDecl *cls);
  static const Field *unionMemberField(const MemberExpr *member, const Type *m,
                                       const ClassDecl *cls);
  static ResultUnionMemberTyped lackingFieldError(const MemberExpr *member, const Type *type,
                                                  const Type *lacking);

  struct UnionMethodPart {
    const Type *type;
    UnionMethod entry;
  };

  using ResultUnionMethodPart = Result<UnionMethodPart, UnionCheckerCheckUnionMethodCallError>;

  static bool isAllObjects(const Type *type);
  ResultUnionMethodPart memberMethod(const MemberExpr *member, const Type *m);
  ResultUnionMethodPart classMemberMethod(const MemberExpr *member, const Type *m, ClassDecl *cls);
  ResultUnionMethodPart classVariantMethod(const MemberExpr *member, const Type *m, ClassDecl *cls,
                                           FunctionDecl *variant);

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  BytesChecker *bytesChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
};

}
