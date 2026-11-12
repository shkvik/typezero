#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class ClassDeclarer;
class TemplateTypes;
class ReturnInferrer;

enum class ExtendsStepKind { GOAL, BIND, CHOICE };

struct ExtendsGoal {
  static constexpr ExtendsStepKind kind = ExtendsStepKind::GOAL;
  const Type *type;
  const Type *pattern;
};

struct BindInfer {
  static constexpr ExtendsStepKind kind = ExtendsStepKind::BIND;
  size_t index;
  const Type *type;
};

struct ExtendsChoice {
  static constexpr ExtendsStepKind kind = ExtendsStepKind::CHOICE;
  const Type *type;
  const Type *pattern;
  size_t next;
  std::vector<const Type *> saved;
};

using ExtendsStep = Union<ExtendsGoal, BindInfer, ExtendsChoice>;

class ExtendsRelation {
public:
  ExtendsRelation(SemanticState &owner, SemanticParts &all);

  bool isExtendingType(const Type *type, const Type *pattern);
  bool isExtendingType(const Type *type, const Type *pattern, InferMatch *inference);

private:
  using Steps = std::vector<ExtendsStep>;

  void bindInfer(const BindInfer &bind, InferMatch *inference) const;
  bool canRetryChoice(Steps &steps, InferMatch *inference) const;
  bool canTryAlternative(ExtendsChoice choice, Steps &steps) const;
  bool isStepHolding(const ExtendsStep &step, InferMatch *inference, Steps &steps);
  bool isGoalHolding(const ExtendsGoal &goal, InferMatch *inference, Steps &steps);
  void pushDefinedGoal(const Type *type, const Type *pattern, Steps &steps) const;
  bool canTryNext(const ExtendsChoice &choice, InferMatch *inference, Steps &steps) const;
  void pushMemberGoals(llvm::ArrayRef<const Type *> types, const Type *pattern, Steps &steps) const;
  bool canStartChoice(const Type *type, const Type *pattern, InferMatch *inference,
                      Steps &steps) const;
  bool isBindingInferred(const Type *type, const Type *pattern, InferMatch *inference,
                         Steps &steps) const;

  bool isDictHolding(const Type *type, const Type *pattern, Steps &steps) const;
  bool isArrayHolding(const Type *type, const Type *pattern, Steps &steps) const;
  bool isResultHolding(const Type *type, const Type *pattern, Steps &steps) const;
  bool isPromiseHolding(const Type *type, const Type *pattern, Steps &steps) const;
  bool isIteratorHolding(const Type *type, const Type *pattern, Steps &steps) const;
  bool isLeafHolding(const Type *type, const Type *pattern, InferMatch *inference, Steps &steps);
  bool isTupleHolding(const Type *type, const Type *pattern, InferMatch *inference, Steps &steps);
  bool isStructureHolding(const Type *type, const Type *pattern, InferMatch *inference,
                          Steps &steps);

  size_t fixedParamCount(const Type *function) const;
  std::vector<const Type *> fieldTypes(const Type *object) const;
  const Type *paramAt(const Type *function, size_t index) const;
  const ClassDecl *genericOrigin(const ClassDecl *classDecl) const;
  const Type *restArguments(const Type *function, size_t from) const;
  std::optional<size_t> soleSpread(llvm::ArrayRef<const Type *> elements) const;
  bool isParamFitting(const Type *my, const Type *their, InferMatch *inference);
  bool isExtendingRest(const Type *type, const Type *pattern, InferMatch *inference);
  bool isExtendingClass(const Type *type, const Type *pattern, InferMatch *inference);
  bool isExtendingTuple(const Type *type, const Type *pattern, InferMatch *inference);
  bool isExtendingObject(const Type *type, const Type *pattern, InferMatch *inference);
  bool isExtendingFunction(const Type *type, const Type *pattern, InferMatch *inference);
  bool isInstanceOf(const Type *candidate, const ClassDecl *generic, size_t typeArgCount) const;
  const Type *memberType(const Type *type, const ClassDecl *classDecl, llvm::StringRef name) const;
  bool isExtendingEach(llvm::ArrayRef<const Type *> types, llvm::ArrayRef<const Type *> patterns,
                       InferMatch *inference);
  bool isExtendingPlainTuple(llvm::ArrayRef<const Type *> mine, const Type *pattern,
                             InferMatch *inference);
  bool isExtendingField(const Type *type, const ClassDecl *classDecl, const Field &field,
                        InferMatch *inference);
  bool isExtendingRestTuple(llvm::ArrayRef<const Type *> mine, llvm::ArrayRef<const Type *> theirs,
                            const Type *restElement, InferMatch *inference);
  bool isExtendingSpreadTuple(llvm::ArrayRef<const Type *> mine,
                              llvm::ArrayRef<const Type *> theirs, size_t spread,
                              InferMatch *inference);

  SemanticState &state;
  ClassDeclarer *classDeclarer = nullptr;
  TemplateTypes *templateTypes = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
};

}
