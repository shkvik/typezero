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

  bool extendsType(const Type *type, const Type *pattern, InferMatch *inference = nullptr);
  bool extendsTuple(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsFunction(const Type *type, const Type *pattern, InferMatch *inference);
  bool extendsObject(const Type *type, const Type *pattern, InferMatch *inference);

private:
  static std::vector<const Type *> distributed(const Type *type);
  static bool retryChoice(std::vector<ExtendsStep> &steps, InferMatch *inference);
  static bool tryAlternative(ExtendsChoice choice, std::vector<ExtendsStep> &steps);
  static void bindInfer(const BindInfer &bind, InferMatch *inference);
  static void pushGoals(llvm::ArrayRef<const Type *> types, const Type *pattern,
                        std::vector<ExtendsStep> &steps);
  static bool pushPairs(llvm::ArrayRef<ExtendsGoal> goals, std::vector<ExtendsStep> &steps);

  bool applyExtendsStep(const ExtendsStep &step, InferMatch *inference,
                        std::vector<ExtendsStep> &steps);
  bool extendsStep(const Type *type, const Type *pattern, InferMatch *inference,
                   std::vector<ExtendsStep> &steps);
  static bool bindsInferred(const Type *type, const Type *pattern, InferMatch *inference,
                            std::vector<ExtendsStep> &steps);
  bool leafStep(const Type *type, const Type *pattern, InferMatch *inference,
                std::vector<ExtendsStep> &steps);
  bool structureStep(const Type *type, const Type *pattern, InferMatch *inference,
                     std::vector<ExtendsStep> &steps);

  SemanticState &state;
  ClassDeclarer *classDeclarer = nullptr;
  TemplateTypes *templateTypes = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
};

}
