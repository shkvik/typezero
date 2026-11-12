#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FieldFacts;
class LiteralTypes;

enum class UndefinedTag {
  KEPT,
  DROPPED,
};

class UnionNarrowing {
public:
  UnionNarrowing(SemanticState &owner, SemanticParts &all);

  void narrowVariable(Flow &flow, const VarSymbol *variable, const Type *type) const;
  std::optional<NarrowTarget> narrowTarget(const Expr *expr) const;
  const Type *targetType(const Flow &flow, const NarrowTarget &target) const;
  void narrowTargetTo(Flow &flow, const NarrowTarget &target, const Type *type) const;
  void addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  llvm::SmallVector<const Type *, 8> membersOf(const Type *type) const;
  const Expr *withoutReadCasts(const Expr *expr) const;
  const VarSymbol *readVariable(const Expr *expr) const;
  const Type *keptType(const Type *type, llvm::function_ref<bool(const Type *)> keep) const;
  const Type *keptUnion(const std::vector<const Type *> &kept) const;
  bool isNarrowingOf(const VarSymbol *variable,
                     const std::pair<const VarSymbol *, const Type *> &narrowed) const;
  std::optional<NarrowTarget> readTarget(const Expr *expr) const;
  std::optional<NarrowTarget> fieldTarget(const Expr *expr) const;
  bool isPathEntry(const FieldPath &path, const std::pair<FieldPath, const Type *> &entry) const;
  void addTruthinessFacts(const MemberExpr *member, Flow &whenTrue, Flow &whenFalse) const;
  void addBinaryFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void narrowKept(Flow &flow, const NarrowTarget &target,
                  llvm::function_ref<bool(const Type *)> keep) const;
  const Type *tagOf(const Type *member, llvm::StringRef name) const;
  bool areAllTagged(const Type *now, llvm::StringRef name) const;
  const Expr *taggedObject(const MemberExpr *member) const;
  std::optional<NarrowTarget> taggedTarget(const MemberExpr *member, const Flow &flow) const;
  bool isTagNarrowable(const MemberExpr *member, const Flow &flow) const;
  void narrowByTag(const MemberExpr *member, Flow &flow,
                   llvm::function_ref<bool(const Type *)> keep, UndefinedTag undefinedTag) const;
  bool isTagKept(llvm::StringRef name, llvm::function_ref<bool(const Type *)> keep,
                 UndefinedTag undefinedTag, const Type *member) const;
  bool isTruthyLiteral(const Type *type) const;
  bool canBeTruthy(const Type *type) const;
  bool canBeFalsy(const Type *type) const;
  bool isArrayMember(const Type *member) const;
  bool isNotArrayMember(const Type *member) const;
  bool isNotBufferMember(const Type *member) const;
  void addArrayFacts(const Expr *arg, Flow &whenTrue, Flow &whenFalse) const;
  void addInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addBufferFacts(const NarrowTarget &target, const Type *classType, Flow &whenTrue,
                      Flow &whenFalse) const;
  bool isOneOf(llvm::ArrayRef<const Type *> matching, const Type *member) const;
  bool isNoneOf(llvm::ArrayRef<const Type *> matching, const Type *member) const;
  void addMemberSetFacts(const NarrowTarget &target, const Type *classType, Flow &whenTrue,
                         Flow &whenFalse) const;
  bool isSubclassMember(const Type *classType, const Type *member) const;
  bool isNotSubclassMember(const Type *classType, const Type *member) const;
  void addClassFacts(const NarrowTarget &target, const Type *classType, Flow &whenTrue,
                     Flow &whenFalse) const;
  bool hasField(llvm::StringRef name, const Type *member) const;
  bool isMissingField(llvm::StringRef name, const Type *member) const;
  void addInFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addEqualityFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;
  bool isTypeOfNamed(llvm::StringRef kind, const Type *member) const;
  bool isNotTypeOfNamed(llvm::StringRef kind, const Type *member) const;
  void addTypeOfFacts(const UnaryExpr *typeOf, const Expr *other, Flow &equal,
                      Flow &different) const;
  const Type *comparedLiteral(const Expr *other) const;
  const MemberExpr *taggedMember(const Expr *side) const;
  bool isConvertibleFrom(const Type *literal, const Type *type) const;
  bool isOtherThan(const Type *literal, const Type *type) const;
  void addLiteralFacts(const Expr *side, const Type *literal, Flow &equal, Flow &different) const;
  bool isLiteralNarrowing(const VarSymbol *variable, const Type *literal, const Flow &flow) const;
  void narrowToLiteral(const NarrowTarget &target, const Type *literal, Flow &equal,
                       Flow &different) const;

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  LiteralTypes *literalTypes = nullptr;
};

}
