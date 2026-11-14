#include "compiler/semantic/code/classes/overload_choice/overload_choice.h"

namespace tz {

OverloadChoice::OverloadChoice(SemanticState &owner, ClassFacts &sharedFacts)
    : choiceRules(owner),
      variantFilter(owner, this->choiceRules),
      candidateRanking(owner, this->choiceRules) {}

void OverloadChoice::chooseVariant(const ChooseVariant &task) {
  return this->variantFilter.chooseVariant(task);
}

void OverloadChoice::rankVariants(const RankVariants &task) {
  return this->candidateRanking.rankVariants(task);
}

void OverloadChoice::nextRankedVariant(const NextRankedVariant &task) {
  return this->candidateRanking.nextRankedVariant(task);
}

void OverloadChoice::afterGenericParams(const AfterGenericParams &task) {
  return this->candidateRanking.afterGenericParams(task);
}

void OverloadChoice::afterInferredArguments(const AfterInferredArguments &task) {
  return this->candidateRanking.afterInferredArguments(task);
}

}
