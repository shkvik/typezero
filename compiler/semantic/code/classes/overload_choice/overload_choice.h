#pragma once

#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/overload_choice/candidate_ranking/candidate_ranking.h"
#include "compiler/semantic/code/classes/overload_choice/choice_rules/choice_rules.h"
#include "compiler/semantic/code/classes/overload_choice/variant_filter/variant_filter.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class OverloadChoice {
public:
  OverloadChoice(SemanticState &owner, ClassFacts &sharedFacts);

  void rankVariants(const RankVariants &task);
  void chooseVariant(const ChooseVariant &task);
  void nextRankedVariant(const NextRankedVariant &task);
  void afterGenericParams(const AfterGenericParams &task);

private:
  ChoiceRules choiceRules;
  VariantFilter variantFilter;
  CandidateRanking candidateRanking;
};

}
