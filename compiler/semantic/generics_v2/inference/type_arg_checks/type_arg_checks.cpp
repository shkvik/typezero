#include "compiler/semantic/generics_v2/inference/type_arg_checks/type_arg_checks.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"

namespace tz {

TypeArgChecks::TypeArgChecks(SemanticState &owner, BindingRules &sharedBindingRules)
    : state(owner),
      bindingRules(sharedBindingRules) {}

void TypeArgChecks::finishTypeArgs(const FinishTypeArgs &task) {
  auto draft = TypeArgsDraft{
      .generic = task.generic,
      .bound = task.bound,
      .loc = task.loc,
      .use = task.use,
      .missing = task.missing,
      .isInferred = true,
      .index = 0,
  };

  return this->nextTypeArgCheck(NextTypeArgCheck{.draft = std::move(draft)});
}

void TypeArgChecks::nextTypeArgCheck(const NextTypeArgCheck &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  FuncSymbol *generic = draft.generic;
  size_t index = draft.index;
  if (index == generic->typeParams.size()) {
    return this->deliverTypeArgs(draft);
  }
  bool hasDefault = index < generic->typeDefaults.size() && generic->typeDefaults[index] != nullptr;
  if (draft.bound[index] != nullptr || !hasDefault) {
    return this->finishTypeArg(std::move(draft));
  }
  auto substitute = SubstituteType{
      .type = generic->typeDefaults[index],
      .params = generic->typeParams,
      .args = draft.bound,
  };
  tasks.pushTask(AfterTypeArgDefault{.draft = std::move(draft)});

  return tasks.pushTask(std::move(substitute));
}

void TypeArgChecks::afterTypeArgDefault(const AfterTypeArgDefault &task) {
  auto draft = task.draft;
  const Type *fallback = this->state.walk.tasks.takeAnswer();
  if (!this->isMentioningOpen(fallback, draft.generic->typeParams, draft.bound)) {
    draft.bound[draft.index] = fallback;
  }

  return this->finishTypeArg(std::move(draft));
}

bool TypeArgChecks::isMentioningOpen(const Type *type, llvm::ArrayRef<const Type *> params,
                                     llvm::ArrayRef<const Type *> bound) const {
  for (size_t index = 0; index < params.size(); ++index) {
    if (bound[index] == nullptr && GenericCheckHelpers::mentionsAnyOf(type, params[index])) {
      return true;
    }
  }

  return false;
}

void TypeArgChecks::finishTypeArg(TypeArgsDraft draft) {
  auto &tasks = this->state.walk.tasks;
  FuncSymbol *generic = draft.generic;
  size_t index = draft.index;
  const Type *bound = draft.bound[index];
  bool isFailed = bound != nullptr && bound->isError();
  bool isSilent = bound == nullptr && draft.missing == MissingTypeArgs::SILENT;
  if (isFailed || isSilent) {
    return this->rejectTypeArg(std::move(draft));
  }
  auto paramKnown = this->paramInferred(generic, bound, index, draft.use);
  if (paramKnown.isErr()) {
    this->state.diagnostics.error(draft.loc, paramKnown.error().message);

    return this->rejectTypeArg(std::move(draft));
  }
  const Type *element = generic->typeParams[index]->iterated();
  if (element != nullptr) {
    auto wanted =
        SubstituteType{.type = element, .params = generic->typeParams, .args = draft.bound};
    tasks.pushTask(AfterIteratedArgFit{.draft = std::move(draft)});
    tasks.pushTask(ElementTypeOf{.type = bound});

    return tasks.pushTask(std::move(wanted));
  }
  auto constraint = CheckConstraint{
      .arg = bound,
      .param = generic->typeParams[index],
      .params = generic->typeParams,
      .args = draft.bound,
  };
  tasks.pushTask(AfterTypeArgConstraint{.draft = std::move(draft)});

  return tasks.pushTask(std::move(constraint));
}

TypeArgChecks::ResultParamInferred TypeArgChecks::paramInferred(const FuncSymbol *generic,
                                                                const Type *bound, size_t index,
                                                                TypeArgUse use) const {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  if (bound == nullptr) {
    auto samples = this->sampleArgs(params);
    bool isCall = use == TypeArgUse::CALL;
    auto message =
        ErrorTextParamInferred::cannot(params[index]->name(), generic->name, isCall, samples);

    return err(GenericsInferenceRegistryParamInferredError{
        .code = GenericsInferenceRegistryParamInferredErrorCode::CANNOT_INFER,
        .message = message,
    });
  }

  return ok(bound);
}

std::string TypeArgChecks::sampleArgs(llvm::ArrayRef<const Type *> params) const {
  auto text = std::string();
  for (const Type *param : params) {
    if (!text.empty()) {
      text += ", ";
    }
    text +=
        param->isListParam() ? ErrorTextTypeArgSample::list() : ErrorTextTypeArgSample::single();
  }

  return text;
}

void TypeArgChecks::afterIteratedArgFit(const AfterIteratedArgFit &task) {
  auto draft = task.draft;
  auto answers = this->state.walk.tasks.takeAnswers(2);
  const Type *wanted = answers[0];
  const Type *elements = answers[1];
  auto matched = this->iterableFits(draft.generic, draft.bound[draft.index], elements, wanted);
  if (matched.isErr()) {
    this->state.diagnostics.error(draft.loc, matched.error().message);

    return this->rejectTypeArg(std::move(draft));
  }

  return this->acceptTypeArg(std::move(draft));
}

TypeArgChecks::ResultIterableMatch TypeArgChecks::iterableFits(const FuncSymbol *generic,
                                                               const Type *bound, const Type *given,
                                                               const Type *wanted) const {
  bool isFitting = given == wanted || wanted->isError() || bound->isError();
  if (!isFitting && given != nullptr) {
    auto message = ErrorTextIterableFits::elements(bound->name(), given->name(), wanted->name(),
                                                   generic->name);

    return err(GenericsInferenceRegistryIterableFitsError{
        .code = GenericsInferenceRegistryIterableFitsErrorCode::WRONG_ELEMENTS,
        .message = message,
    });
  }
  if (!isFitting) {
    auto message = ErrorTextIterableFits::notIter(bound->name(), wanted->name(), generic->name);

    return err(GenericsInferenceRegistryIterableFitsError{
        .code = GenericsInferenceRegistryIterableFitsErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(bound);
}

void TypeArgChecks::afterTypeArgConstraint(const AfterTypeArgConstraint &task) {
  auto draft = task.draft;
  bool isSatisfied = this->state.walk.tasks.takeVerdict();
  auto met = this->constraintMet(draft.generic, draft.bound, draft.index, isSatisfied);
  if (met.isErr()) {
    this->state.diagnostics.error(draft.loc, met.error().message);

    return this->rejectTypeArg(std::move(draft));
  }

  return this->acceptTypeArg(std::move(draft));
}

TypeArgChecks::ResultConstraintMet TypeArgChecks::constraintMet(const FuncSymbol *generic,
                                                                llvm::ArrayRef<const Type *> bound,
                                                                size_t index,
                                                                bool isSatisfied) const {
  if (!isSatisfied) {
    const Type *param = generic->typeParams[index];
    llvm::ArrayRef<TypeParamDecl> decls = generic->decl->typeParams;
    const TypeParamDecl *written = index < decls.size() ? &decls[index] : nullptr;
    auto constraint = GenericCheckHelpers::constraintSpelling(param, written);
    auto message = ErrorTextConstraintMet::unsatisfied(bound[index]->name(), constraint,
                                                       param->name(), generic->name);

    return err(GenericsInferenceRegistryConstraintMetError{
        .code = GenericsInferenceRegistryConstraintMetErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(bound[index]);
}

void TypeArgChecks::acceptTypeArg(TypeArgsDraft draft) {
  draft.index += 1;

  return this->state.walk.tasks.pushTask(NextTypeArgCheck{.draft = std::move(draft)});
}

void TypeArgChecks::rejectTypeArg(TypeArgsDraft draft) {
  draft.bound[draft.index] = Type::error();
  draft.isInferred = false;
  draft.index += 1;

  return this->state.walk.tasks.pushTask(NextTypeArgCheck{.draft = std::move(draft)});
}

void TypeArgChecks::deliverTypeArgs(const TypeArgsDraft &draft) {
  if (!draft.isInferred) {
    return this->state.walk.tasks.pushVerdict(false);
  }

  return this->bindingRules.pushBound(draft.bound);
}

}
