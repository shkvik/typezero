#include "compiler/semantic/generics_v2/inference/contextual_arguments/contextual_arguments.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

ContextualArguments::ContextualArguments(SemanticState &owner, BindingRules &sharedBindingRules)
    : state(owner),
      bindingRules(sharedBindingRules) {}

void ContextualArguments::nextContextualArgument(const NextContextualArgument &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (task.position == draft.contextual.size()) {
    return this->bindingRules.finishBinding(std::move(draft));
  }
  size_t index = draft.contextual[task.position];
  auto substitute = SubstituteType{
      .type = this->bindingRules.parameterFor(draft, index),
      .params = draft.generic->typeParams,
      .args = draft.bound,
  };
  tasks.pushTask(AfterContextualPartial{.draft = std::move(draft), .position = task.position});

  return tasks.pushTask(std::move(substitute));
}

void ContextualArguments::afterContextualPartial(const AfterContextualPartial &task) {
  const Type *partial = this->state.walk.tasks.takeAnswer();
  const Type *function = partial->nonOptional();
  if (function->isFunction()) {
    return this->walkCallbackArgument(task.draft, task.position, function);
  }

  return this->walkArgumentWithDefaults(task.draft, task.position, partial);
}

void ContextualArguments::walkCallbackArgument(GenericBindingDraft draft, size_t position,
                                               const Type *function) {
  auto &tasks = this->state.walk.tasks;
  size_t index = draft.contextual[position];
  Expr **slot = &draft.call->args[index];
  auto signature = this->callbackSignature(draft.generic->typeParams, function);
  auto *name = dyn_cast<NameExpr>(*slot);
  FuncSymbol *other = name != nullptr ? this->state.walk.scopes.findGenericFunction(name) : nullptr;
  if (other == nullptr) {
    this->walkCallbackParams(draft, index, function);
  }
  tasks.pushTask(AfterContextualArgument{.draft = std::move(draft), .position = position});

  return this->pushCallbackValue(slot, other, signature);
}

void ContextualArguments::pushCallbackValue(Expr **slot, FuncSymbol *other,
                                            const Signature &signature) {
  auto &tasks = this->state.walk.tasks;
  auto *closure = dyn_cast<FunctionExpr>(*slot);
  if (closure != nullptr) {
    tasks.pushTask(WriteCheckedSlot{.slot = slot});
    return tasks.pushTask(CheckFunctionExpr{.functionExpr = closure, .expected = signature});
  }
  if (other != nullptr) {
    tasks.pushTask(WriteCheckedSlot{.slot = slot});
    return tasks.pushTask(
        CheckGenericValue{.name = cast<NameExpr>(*slot), .generic = other, .wanted = signature});
  }

  return tasks.pushTask(CheckBuiltinCallbackSlot{.slot = slot, .expected = signature});
}

Signature ContextualArguments::callbackSignature(llvm::ArrayRef<const Type *> openParams,
                                                 const Type *function) const {
  Signature signature;
  for (const Type *param : function->params()) {
    bool isOpen = GenericCheckHelpers::mentionsAnyOf(param, openParams);
    signature.params.push_back(isOpen ? Type::error() : param);
  }
  bool isOpenResult = GenericCheckHelpers::mentionsAnyOf(function->ret(), openParams);
  signature.minArgs = function->minArgs();
  signature.ret = isOpenResult ? nullptr : function->ret();
  signature.voidResult = function->ret()->isVoid();

  return signature;
}

void ContextualArguments::walkCallbackParams(GenericBindingDraft &draft, size_t index,
                                             const Type *function) {
  auto *closure = dyn_cast<FunctionExpr>(draft.call->args[index]);
  for (size_t position = 0; position < function->params().size(); ++position) {
    bool isWritten = closure != nullptr && position < closure->fn->params.size() &&
                     closure->fn->params[position].type.has_value();
    if (!isWritten) {
      this->walkCallbackParam(draft, index, function->params()[position], position);
    }
  }
}

void ContextualArguments::walkCallbackParam(GenericBindingDraft &draft, size_t index,
                                            const Type *callbackParam, size_t position) {
  const Expr *arg = draft.call->args[index];
  auto *closure = dyn_cast<FunctionExpr>(arg);
  llvm::ArrayRef<const Type *> params = draft.generic->typeParams;
  for (size_t typeIndex = 0; typeIndex < params.size(); ++typeIndex) {
    const Type *param = params[typeIndex];
    bool isMentioned = GenericCheckHelpers::mentionsAnyOf(callbackParam, param);
    auto known = this->callbackParam(draft.generic, param, draft.bound[typeIndex], isMentioned,
                                     closure, position);
    if (known.isErr()) {
      this->state.diagnostics.error(arg->loc(), known.error().message);
      draft.bound[typeIndex] = Type::error();
      draft.isInferred = false;
    }
  }
}

ContextualArguments::ResultCallbackParam
ContextualArguments::callbackParam(const FuncSymbol *generic, const Type *param, const Type *bound,
                                   bool isMentioned, const FunctionExpr *functionExpr,
                                   size_t position) const {
  if (bound == nullptr && isMentioned) {
    bool isNamed = functionExpr != nullptr && position < functionExpr->fn->params.size();
    std::string what = isNamed
                           ? ErrorTextCallbackParam::param(functionExpr->fn->params[position].name)
                           : ErrorTextCallbackParam::params();
    auto message = ErrorTextCallbackParam::uninferred(param->name(), generic->name, what,
                                                      functionExpr != nullptr);

    return err(GenericsInferenceRegistryCallbackParamError{
        .code = GenericsInferenceRegistryCallbackParamErrorCode::CALLBACK_PARAM_UNINFERRED,
        .message = message,
    });
  }

  return ok(param);
}

void ContextualArguments::walkArgumentWithDefaults(GenericBindingDraft draft, size_t position,
                                                   const Type *partial) {
  auto &tasks = this->state.walk.tasks;
  bool isOpen = GenericCheckHelpers::mentionsAnyOf(partial, draft.generic->typeParams);
  bool isDefaulted = isOpen && this->fillTypeDefaults(draft);
  if (!isDefaulted) {
    return this->walkContextualNow(std::move(draft), position, partial);
  }
  size_t index = draft.contextual[position];
  auto substitute = SubstituteType{
      .type = this->bindingRules.parameterFor(draft, index),
      .params = draft.generic->typeParams,
      .args = draft.bound,
  };
  tasks.pushTask(AfterDefaultedHint{.draft = std::move(draft), .position = position});

  return tasks.pushTask(std::move(substitute));
}

bool ContextualArguments::fillTypeDefaults(GenericBindingDraft &draft) {
  bool isDefaulted = false;
  for (size_t index = 0; index < draft.bound.size(); ++index) {
    const auto *fallback = this->typeDefault(draft.generic, index);
    if (draft.bound[index] == nullptr && fallback != nullptr) {
      draft.bound[index] = fallback;
      isDefaulted = true;
    }
  }

  return isDefaulted;
}

const Type *ContextualArguments::typeDefault(const FuncSymbol *generic, size_t index) const {
  bool hasDefault = index < generic->typeDefaults.size() && generic->typeDefaults[index] != nullptr;
  if (!hasDefault) {
    return nullptr;
  }
  const Type *fallback = generic->typeDefaults[index];
  bool isClosed = !GenericCheckHelpers::mentionsAnyOf(fallback, generic->typeParams);

  return isClosed ? fallback : nullptr;
}

void ContextualArguments::afterDefaultedHint(const AfterDefaultedHint &task) {
  const Type *hint = this->state.walk.tasks.takeAnswer();

  return this->walkContextualNow(task.draft, task.position, hint);
}

void ContextualArguments::walkContextualNow(GenericBindingDraft draft, size_t position,
                                            const Type *hint) {
  auto &tasks = this->state.walk.tasks;
  size_t index = draft.contextual[position];
  Expr **slot = &draft.call->args[index];
  bool isStillOpen = GenericCheckHelpers::mentionsAnyOf(hint, draft.generic->typeParams);
  tasks.pushTask(AfterContextualArgument{.draft = std::move(draft), .position = position});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = isStillOpen ? nullptr : hint});
}

void ContextualArguments::afterContextualArgument(const AfterContextualArgument &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  this->bindingRules.takeLookups(*draft.inference, draft.pending);
  size_t index = draft.contextual[task.position];
  draft.checked[index] = true;
  const auto *pattern = this->bindingRules.parameterFor(draft, index);
  CandidateInference more(draft.generic->typeParams, this->state.generics.aliases,
                          IterationLookups::ELEMENTS);
  more.copyLookups(*draft.inference);
  more.from(pattern, draft.call->args[index]->type);
  auto lookups = this->bindingRules.missingLookups(more);
  if (this->bindingRules.hasLookups(lookups)) {
    draft.pending = lookups;
    tasks.pushTask(AfterContextualArgument{.draft = std::move(draft), .position = task.position});

    return this->bindingRules.pushLookups(lookups);
  }
  this->adoptCandidates(draft, more);

  return tasks.pushTask(
      NextContextualArgument{.draft = std::move(draft), .position = task.position + 1});
}

void ContextualArguments::adoptCandidates(GenericBindingDraft &draft,
                                          const CandidateInference &more) {
  for (size_t typeIndex = 0; typeIndex < draft.bound.size(); ++typeIndex) {
    if (draft.bound[typeIndex] == nullptr) {
      draft.inference->adoptCandidates(typeIndex, more);
      this->bindingRules.settleTypeParam(draft, typeIndex);
    }
  }
}

}
