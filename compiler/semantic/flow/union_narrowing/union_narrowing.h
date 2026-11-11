#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FieldFacts;
class LiteralTypes;

class UnionNarrowing {
public:
  UnionNarrowing(SemanticState &owner, SemanticParts &all);

  static void narrowVariable(Flow &flow, const VarSymbol *var, const Type *type);
  std::optional<NarrowTarget> narrowTarget(const Expr *expr) const;
  const Type *targetType(const Flow &flow, const NarrowTarget &target) const;
  void narrowTargetTo(Flow &flow, const NarrowTarget &target, const Type *type) const;
  void addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);
  static const VarSymbol *readVariable(const Expr *expr);
  static const Type *keepMembers(const Type *type, llvm::function_ref<bool(const Type *)> keep);
  void narrowKept(Flow &flow, const NarrowTarget &target,
                  llvm::function_ref<bool(const Type *)> keep) const;
  static const Type *tagOf(const Type *m, llvm::StringRef name);
  static bool allTagged(const Type *now, llvm::StringRef name);
  bool narrowByTag(const MemberExpr *member, Flow &flow,
                   llvm::function_ref<bool(const Type *)> keep, bool undefinedKept) const;
  static bool mayBe(const Type *t, bool truthy);
  static bool mayBeTruthy(const Type *t);
  static bool mayBeFalsy(const Type *t);
  static bool isArrayMember(const Type *m);
  static bool isNotArrayMember(const Type *m);
  static bool isNotBufferMember(const Type *m);
  void addArrayFacts(const Expr *arg, Flow &whenTrue, Flow &whenFalse) const;
  void addInstanceOfFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const;
  void addBufferFacts(const NarrowTarget &target, const Type *cls, Flow &whenTrue,
                      Flow &whenFalse) const;
  void addMemberSetFacts(const NarrowTarget &target, const Type *cls, Flow &whenTrue,
                         Flow &whenFalse) const;
  void addClassFacts(const NarrowTarget &target, const Type *cls, Flow &whenTrue,
                     Flow &whenFalse) const;
  static bool hasField(const Type *m, llvm::StringRef name);
  void addInFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const;
  void addEqualityFacts(const Expr *side, const Expr *other, Flow &equal, Flow &different) const;
  static bool givesTypeOf(const Type *m, llvm::StringRef kind);
  void addTypeOfFacts(const UnaryExpr *typeOf, const Expr *other, Flow &equal,
                      Flow &different) const;
  static const Type *comparedLiteral(const Expr *other);
  static const MemberExpr *taggedMember(const Expr *side);
  void addLiteralFacts(const Expr *side, const Type *literal, Flow &equal, Flow &different) const;
  void narrowToLiteral(const NarrowTarget &target, const Type *literal, Flow &equal,
                       Flow &different) const;
  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  LiteralTypes *literalTypes = nullptr;
};

}
