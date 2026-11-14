#include "compiler/semantic/generics_v2/inference/argument_patterns/argument_patterns.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"

namespace tz {

ArgumentPatterns::ArgumentPatterns(SemanticState &owner, BindingRules &sharedBindingRules)
    : state(owner),
      bindingRules(sharedBindingRules) {}

void ArgumentPatterns::inferFromArguments(const InferFromArguments &task) {
  FuncSymbol *generic = task.function;
  auto draft = InferredArgumentsDraft{
      .generic = generic,
      .args = task.args,
      .inference = CandidateInference(generic->typeParams, this->state.generics.aliases,
                                      IterationLookups::ELEMENTS_AND_ASYNC),
      .pending = {},
      .bound = std::vector<const Type *>(generic->typeParams.size(), nullptr),
      .index = 0,
  };

  return this->inferArgumentPatterns(InferArgumentPatterns{.draft = std::move(draft)});
}

void ArgumentPatterns::inferArgumentPatterns(const InferArgumentPatterns &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  this->bindingRules.takeLookups(*draft.inference, draft.pending);
  auto next = *draft.inference;
  this->inferPatterns(next, draft.generic, draft.args);
  auto lookups = this->bindingRules.missingLookups(next);
  if (this->bindingRules.hasLookups(lookups)) {
    draft.pending = lookups;
    tasks.pushTask(InferArgumentPatterns{.draft = std::move(draft)});

    return this->bindingRules.pushLookups(lookups);
  }
  for (size_t index = 0; index < draft.bound.size(); ++index) {
    auto candidate = this->argumentCandidate(next, index);
    if (!candidate.has_value()) {
      return tasks.pushVerdict(false);
    }
    draft.bound[index] = *candidate;
  }

  return this->nextFittingArgument(NextFittingArgument{.draft = std::move(draft)});
}

void ArgumentPatterns::inferPatterns(CandidateInference &inference, const FuncSymbol *generic,
                                     llvm::ArrayRef<Expr *> args) const {
  for (size_t index = 0; index < args.size(); ++index) {
    const auto *pattern = this->argumentPattern(generic, index);
    if (pattern != nullptr && args[index]->type != nullptr) {
      inference.from(pattern, args[index]->type);
    }
  }
}

const Type *ArgumentPatterns::argumentPattern(const FuncSymbol *generic, size_t index) const {
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  if (index < fixed) {
    return generic->params[index];
  }
  if (generic->hasRest && generic->params.back()->isArray()) {
    return generic->params.back()->inner();
  }

  return nullptr;
}

std::optional<const Type *> ArgumentPatterns::argumentCandidate(const CandidateInference &inference,
                                                                size_t index) const {
  const Type *type = nullptr;
  for (const Type *candidate : inference.getCandidates(index)) {
    type = type != nullptr ? this->state.types.relations.unify(type, candidate) : candidate;
    if (type == nullptr) {
      return std::nullopt;
    }
  }
  if (!inference.hasSeenUndefined(index)) {
    return type;
  }

  return type != nullptr ? Type::optional(type) : Type::voidTy();
}

void ArgumentPatterns::nextFittingArgument(const NextFittingArgument &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  FuncSymbol *generic = draft.generic;
  size_t index = draft.index;
  if (index == draft.bound.size()) {
    return this->bindingRules.pushBound(draft.bound);
  }
  bool hasDefault = index < generic->typeDefaults.size() && generic->typeDefaults[index] != nullptr;
  if (draft.bound[index] != nullptr || !hasDefault) {
    return this->fitArgument(std::move(draft));
  }
  auto substitute = SubstituteType{
      .type = generic->typeDefaults[index],
      .params = generic->typeParams,
      .args = draft.bound,
  };
  tasks.pushTask(AfterFittingDefault{.draft = std::move(draft)});

  return tasks.pushTask(std::move(substitute));
}

void ArgumentPatterns::afterFittingDefault(const AfterFittingDefault &task) {
  auto draft = task.draft;
  draft.bound[draft.index] = this->state.walk.tasks.takeAnswer();

  return this->fitArgument(std::move(draft));
}

void ArgumentPatterns::fitArgument(InferredArgumentsDraft draft) {
  auto &tasks = this->state.walk.tasks;
  llvm::ArrayRef<const Type *> params = draft.generic->typeParams;
  const Type *bound = draft.bound[draft.index];
  if (bound == nullptr || GenericCheckHelpers::mentionsAnyOf(bound, params)) {
    return tasks.pushVerdict(false);
  }
  auto constraint = CheckConstraint{
      .arg = bound,
      .param = params[draft.index],
      .params = params,
      .args = draft.bound,
  };
  tasks.pushTask(AfterFittingConstraint{.draft = std::move(draft)});

  return tasks.pushTask(std::move(constraint));
}

void ArgumentPatterns::afterFittingConstraint(const AfterFittingConstraint &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (!tasks.takeVerdict()) {
    return tasks.pushVerdict(false);
  }
  draft.index += 1;

  return tasks.pushTask(NextFittingArgument{.draft = std::move(draft)});
}

}
