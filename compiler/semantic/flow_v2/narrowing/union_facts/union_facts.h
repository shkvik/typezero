#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include <optional>
#include <utility>
#include <vector>

namespace tz {

class SemanticState;

enum class UndefinedTag {
  KEPT,
  DROPPED,
};

class UnionFacts {
public:
  explicit UnionFacts(SemanticState &owner);

  void addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  void narrowVariable(Flow &flow, const VarSymbol *variable, const Type *type) const;

private:
  void addArrayFacts(const Expr *arg, Flow &whenTrue, Flow &whenFalse) const;
  void addInFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void narrowTargetTo(Flow &flow, const NarrowTarget &target, const Type *type) const;
  void addBinaryFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;
  void addTruthinessFacts(const MemberExpr *member, Flow &whenTrue, Flow &whenFalse) const;
  void addEqualityFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;
  void addLiteralFacts(const Expr *side, const Type *literal, Flow &equal, Flow &different) const;
  void addTypeOfFacts(const UnaryExpr *typeOf, const Expr *other, Flow &equal,
                      Flow &different) const;
  void narrowToLiteral(const NarrowTarget &target, const Type *literal, Flow &equal,
                       Flow &different) const;
  void narrowKept(Flow &flow, const NarrowTarget &target,
                  llvm::function_ref<bool(const Type *)> keep) const;
  void addClassFacts(const NarrowTarget &target, const Type *classType, Flow &whenTrue,
                     Flow &whenFalse) const;
  void addBufferFacts(const NarrowTarget &target, const Type *classType, Flow &whenTrue,
                      Flow &whenFalse) const;
  void addMemberSetFacts(const NarrowTarget &target, const Type *classType, Flow &whenTrue,
                         Flow &whenFalse) const;
  void narrowByTag(const MemberExpr *member, Flow &flow,
                   llvm::function_ref<bool(const Type *)> keep, UndefinedTag undefinedTag) const;


  bool canBeFalsy(const Type *type) const;
  bool canBeTruthy(const Type *type) const;
  bool isArrayMember(const Type *member) const;
  bool isTruthyLiteral(const Type *type) const;
  bool isNotArrayMember(const Type *member) const;
  bool isNotBufferMember(const Type *member) const;
  bool hasField(llvm::StringRef name, const Type *member) const;
  bool isOtherThan(const Type *literal, const Type *type) const;
  bool areAllTagged(const Type *now, llvm::StringRef name) const;
  bool isTypeOfNamed(llvm::StringRef kind, const Type *member) const;
  bool isConvertibleFrom(const Type *literal, const Type *type) const;
  bool isMissingField(llvm::StringRef name, const Type *member) const;
  bool isNotTypeOfNamed(llvm::StringRef kind, const Type *member) const;
  bool isSubclassMember(const Type *classType, const Type *member) const;
  bool isTagNarrowable(const MemberExpr *member, const Flow &flow) const;
  bool isNotSubclassMember(const Type *classType, const Type *member) const;
  bool isOneOf(llvm::ArrayRef<const Type *> matching, const Type *member) const;
  bool isNoneOf(llvm::ArrayRef<const Type *> matching, const Type *member) const;
  bool isPathEntry(const FieldPath &path, const std::pair<FieldPath, const Type *> &entry) const;
  bool isLiteralNarrowing(const VarSymbol *variable, const Type *literal, const Flow &flow) const;
  bool isNarrowingOf(const VarSymbol *variable,
                     const std::pair<const VarSymbol *, const Type *> &narrowed) const;
  bool isTagKept(llvm::StringRef name, llvm::function_ref<bool(const Type *)> keep,
                 UndefinedTag undefinedTag, const Type *member) const;


  const Expr *withoutReadCasts(const Expr *expr) const;
  const Expr *taggedObject(const MemberExpr *member) const;


  const Type *comparedLiteral(const Expr *other) const;
  const Type *tagOf(const Type *member, llvm::StringRef name) const;
  const Type *keptUnion(const std::vector<const Type *> &kept) const;
  const Type *targetType(const Flow &flow, const NarrowTarget &target) const;
  const Type *keptType(const Type *type, llvm::function_ref<bool(const Type *)> keep) const;


  std::optional<NarrowTarget> readTarget(const Expr *expr) const;
  std::optional<NarrowTarget> fieldTarget(const Expr *expr) const;
  std::optional<NarrowTarget> narrowTarget(const Expr *expr) const;
  std::optional<NarrowTarget> taggedTarget(const MemberExpr *member, const Flow &flow) const;


  const VarSymbol *readVariable(const Expr *expr) const;
  const MemberExpr *taggedMember(const Expr *side) const;
  llvm::SmallVector<const Type *, 8> membersOf(const Type *type) const;

  SemanticState &state;
};

}
