#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/code/unions/code_unions_registry_errors.h"
#include "compiler/semantic/code/unions/union_rules/union_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class UnionMembers {
public:
  UnionMembers(SemanticState &owner, UnionRules &sharedUnionRules);

  void checkUnionMember(const CheckUnionMember &task);
  void checkUnionFieldTarget(const CheckUnionFieldTarget &task);

private:
  struct TagHint {
    std::string text;
    bool isUnique;
  };

  using ResultMemberAllowed = Result<void, CodeUnionsRegistryCheckUnionMemberError>;
  using ResultTargetAllowed = Result<void, CodeUnionsRegistryCheckUnionFieldTargetError>;
  using ResultMemberFieldType = Result<const Type *, CodeUnionsRegistryCheckUnionMemberError>;
  using ResultUnionFieldTarget = Result<const Type *, CodeUnionsRegistryCheckUnionFieldTargetError>;
  using ResultMemberFieldList =
      Result<std::vector<const Type *>, CodeUnionsRegistryCheckUnionMemberError>;

  void reportUnionMember(MemberExpr *member, const std::string &message);


  bool isAllBytes(const Type *type) const;
  bool isIdentifierChar(char letter) const;
  bool isLiteralSet(const Type *type) const;
  bool isTagField(const Type *type, llvm::StringRef tagName) const;
  bool isHoldingLiteral(const Type *set, const Type *literal) const;
  bool isUnknownToPlainObjects(const MemberExpr *member, const Type *type) const;
  bool isTagUnique(const MemberExpr *member, const Type *type, llvm::StringRef tagName,
                   const Type *tag) const;


  const Type *lastHolder(const MemberExpr *member, const Type *type) const;
  const Type *taggedHolder(const MemberExpr *member, const Type *type,
                           llvm::StringRef tagName) const;


  std::string fieldText(const Expr *object, llvm::StringRef field) const;
  std::string holderHint(const MemberExpr *member, const Type *type) const;
  std::string lackingHint(const MemberExpr *member, const Type *type) const;
  const Field *assignableField(const MemberExpr *member, const Type *memberType,
                               const ClassDecl *classDecl) const;
  const Field *unionMemberField(const MemberExpr *member, const Type *memberType,
                                const ClassDecl *classDecl) const;


  ResultMemberAllowed checkNotTagged(const MemberExpr *member, const Type *type) const;
  ResultMemberAllowed checkFieldKnown(const MemberExpr *member, const Type *type) const;
  ResultMemberAllowed checkFieldAccessible(const MemberExpr *member, const ClassDecl *classDecl);
  ResultTargetAllowed checkTargetAccessible(const MemberExpr *member, const ClassDecl *classDecl);
  ResultMemberAllowed checkJoinable(const MemberExpr *member, const Type *type,
                                    const Type *joined) const;
  ResultTargetAllowed checkSameTarget(const MemberExpr *member, const Type *type,
                                      const Type *common, const Type *fieldType) const;


  ResultMemberFieldList memberFieldTypes(const MemberExpr *member, const Type *type);
  ResultMemberFieldType memberFieldType(const MemberExpr *member, const Type *type,
                                        const Type *memberType);


  ResultUnionFieldTarget unionFieldTarget(MemberExpr *member);
  std::optional<TagHint> tagHint(const MemberExpr *member, const Type *type,
                                 llvm::StringRef tagName) const;
  ResultUnionFieldTarget memberTargetType(const MemberExpr *member, const Type *type,
                                          const Type *memberType);


  std::optional<std::string> chainText(const Expr *expr) const;

  SemanticState &state;
  UnionRules &unionRules;
};

}
