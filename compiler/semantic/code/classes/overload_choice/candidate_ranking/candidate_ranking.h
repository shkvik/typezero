#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/code/classes/overload_choice/choice_rules/choice_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/union/union/union.h"
#include <optional>
#include <utility>
#include <vector>

namespace tz {

enum class RankForm {
  SAME,
  OPTIONAL_SAME,
  OPTIONAL_WRAP,
  WORST_MEMBER,
  BEST_TARGET,
  RESULT,
  FIELDS
};

enum class RankStepKind { GOAL, JOIN };

struct RankGoal {
  static constexpr RankStepKind kind = RankStepKind::GOAL;
  const Type *from;
  const Type *to;
};

struct RankJoin {
  static constexpr RankStepKind kind = RankStepKind::JOIN;
  const Type *from;
  const Type *to;
  RankForm form;
  std::vector<RankGoal> parts;
};

using RankStep = Union<RankGoal, RankJoin>;

struct RankRun {
  std::vector<RankStep> steps;
  std::vector<int> answers;
  std::vector<std::pair<const Type *, const Type *>> path;
};

class CandidateRanking {
public:
  CandidateRanking(SemanticState &owner, ChoiceRules &sharedChoiceRules);

  void rankVariants(const RankVariants &task);
  void nextRankedVariant(const NextRankedVariant &task);
  void afterGenericParams(const AfterGenericParams &task);

private:
  using ResultChoiceAllowed = Result<void, CodeClassesRegistryChooseVariantError>;
  using ResultBestCandidate = Result<VariantCandidate, CodeClassesRegistryChooseVariantError>;

  void finishChoice(const ChoiceDraft &draft);
  void enterRank(const RankGoal &goal, RankRun &run) const;
  void joinRanks(const RankJoin &join, RankRun &run) const;
  void pushGenericParams(ChoiceDraft draft, std::vector<const Type *> typeArgs);
  void noteMismatch(const FunctionDecl *variant, const Expr *arg, const Type *param, size_t index,
                    VariantRanking &ranking) const;
  void rankVariant(FunctionDecl *variant, llvm::ArrayRef<Expr *> args,
                   llvm::ArrayRef<const Type *> params, const std::vector<const Type *> &typeArgs,
                   VariantRanking &ranking) const;


  bool isShapeObject(const Type *type) const;
  bool isOpaqueRanked(const Type *type) const;
  bool isSameShape(const Type *from, const Type *to) const;
  bool hasSameFields(const Type *from, const Type *to) const;
  bool isBetter(const VariantCandidate &candidate, const VariantCandidate &other) const;
  bool isBetterThanOthers(const VariantCandidate &candidate,
                          llvm::ArrayRef<VariantCandidate> applicable) const;


  int jsonRank(const Type *from) const;
  int bestRank(llvm::ArrayRef<int> ranks) const;
  int worstRank(llvm::ArrayRef<int> ranks) const;
  int bytesRank(const Type *from, const Type *to) const;
  int matchRank(const Type *from, const Type *to) const;
  int iteratorRank(const Type *from, const Type *to) const;
  int conversionRank(const Type *from, const Type *to) const;
  int subtypeDistance(const Type *from, const Type *to) const;
  int halfRank(const Type *from, const Type *to, int rank) const;
  int joinedRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const;
  int resultRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const;
  int argumentRank(const Expr *arg, const Type *param, VariantCandidate &candidate) const;


  RankJoin rankJoin(const Type *from, const Type *to) const;
  RankJoin resultJoin(const Type *from, const Type *to) const;
  RankJoin fieldsJoin(const Type *from, const Type *to) const;
  RankJoin membersJoin(const Type *from, const Type *to) const;
  RankJoin targetsJoin(const Type *from, const Type *to) const;
  RankJoin optionalJoin(const Type *from, const Type *to) const;


  std::optional<int> rankLeaf(const Type *from, const Type *to) const;
  std::optional<int> objectRank(const Type *from, const Type *to) const;


  ResultChoiceAllowed checkArgumentsTyped(llvm::ArrayRef<Expr *> args) const;
  ResultChoiceAllowed checkRankedCandidates(llvm::StringRef what,
                                            const VariantRanking &ranking) const;
  ResultBestCandidate checkSoleBest(llvm::StringRef what,
                                    llvm::ArrayRef<VariantCandidate> applicable) const;
  ResultChoiceAllowed checkGenericAllowed(const VariantCandidate &candidate,
                                          GenericVariants generics) const;


  std::vector<const Type *> plainParams(const FuncSymbol *function, size_t count) const;

  SemanticState &state;
  ChoiceRules &choiceRules;
};

}
