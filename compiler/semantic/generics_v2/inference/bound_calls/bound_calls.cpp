#include "compiler/semantic/generics_v2/inference/bound_calls/bound_calls.h"
#include "compiler/program/types/argument_lists/argument_lists.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/types/type_parts/type_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

BoundCalls::BoundCalls(SemanticState &owner, BindingRules &sharedBindingRules)
    : state(owner),
      bindingRules(sharedBindingRules) {}

void BoundCalls::afterBindingTypeArgs(const AfterBindingTypeArgs &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  bool isFinished = tasks.takeVerdict();
  if (isFinished) {
    draft.bound = tasks.takeAnswers(draft.generic->typeParams.size());
  }
  draft.isInferred = isFinished && draft.isInferred;
  this->walkGrowingArgs(draft);
  if (!draft.isInferred) {
    this->walkUncheckedArguments(draft);

    return this->bindingRules.deliverFailure(draft);
  }

  return this->instantiateCall(std::move(draft));
}

void BoundCalls::walkGrowingArgs(GenericBindingDraft &draft) {
  llvm::ArrayRef<const Type *> params = draft.generic->typeParams;
  for (size_t index = 0; index < params.size() && draft.isInferred; ++index) {
    auto growing = this->notGrowing(draft.generic, draft.bound[index], params[index]);
    if (growing.isErr()) {
      this->state.diagnostics.error(draft.loc, growing.error().message);
      draft.isInferred = false;
    }
  }
}

BoundCalls::ResultArgNotGrowing BoundCalls::notGrowing(const FuncSymbol *generic, const Type *bound,
                                                       const Type *param) const {
  if (this->isGrowing(bound, generic->typeParams)) {
    auto message = ErrorTextNotGrowing::grows(generic->name, bound->name(), param->name());

    return err(GenericsInferenceRegistryNotGrowingError{
        .code = GenericsInferenceRegistryNotGrowingErrorCode::GROWING_TYPE_ARGUMENT,
        .message = message,
    });
  }

  return ok(bound);
}

bool BoundCalls::isGrowing(const Type *type, llvm::ArrayRef<const Type *> params) const {
  const Type *bare = type;
  while (bare->isOptional()) {
    bare = bare->inner();
  }
  if (this->isGrowingAt(bare, params, GrowthPlace::TOP)) {
    return true;
  }
  auto parts = TypeParts::collect(bare, kGrowsRoute);
  for (const Type *part : parts.getTypes()) {
    if (part != bare && this->isGrowingAt(part, params, GrowthPlace::INSIDE)) {
      return true;
    }
  }

  return false;
}

bool BoundCalls::isGrowingAt(const Type *type, llvm::ArrayRef<const Type *> params,
                             GrowthPlace place) const {
  if (type->isParam()) {
    return place == GrowthPlace::INSIDE && llvm::is_contained(params, type);
  }
  bool isHolder = type->isObject() || type->isIterator() || type->isDict();

  return isHolder && GenericCheckHelpers::mentionsAnyOf(type, params);
}

void BoundCalls::walkUncheckedArguments(const GenericBindingDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  CallExpr *call = draft.call;
  for (size_t index = call->args.size(); index > 0; --index) {
    if (!draft.checked[index - 1]) {
      tasks.pushTask(CheckExprSlot{.slot = &call->args[index - 1], .expected = nullptr});
    }
  }
}

void BoundCalls::instantiateCall(GenericBindingDraft draft) {
  auto &tasks = this->state.walk.tasks;
  FuncSymbol *generic = draft.generic;
  this->state.generics.copies.enterInstanceLoc(draft.loc);
  auto list = TypeListDraft{
      .types = {},
      .isValues = {},
      .params = generic->typeParams,
      .args = draft.bound,
      .index = 0,
      .results = {},
  };
  for (const Type *param : generic->params) {
    list.types.push_back(param);
    list.isValues.push_back(true);
  }
  tasks.pushTask(AfterBoundParams{.draft = std::move(draft)});

  return tasks.pushTask(SubstituteTypeList{.draft = std::move(list)});
}

void BoundCalls::afterBoundParams(const AfterBoundParams &task) {
  auto draft = task.draft;
  draft.params = this->state.walk.tasks.takeAnswers(draft.generic->params.size());

  return this->nextLiteralArgument(NextLiteralArgument{.draft = std::move(draft), .position = 0});
}

void BoundCalls::nextLiteralArgument(const NextLiteralArgument &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (task.position == draft.literals.size()) {
    return this->substituteBoundResult(std::move(draft));
  }
  size_t index = draft.literals[task.position];
  auto substitute = SubstituteValueType{
      .type = this->bindingRules.parameterFor(draft, index),
      .params = draft.generic->typeParams,
      .args = draft.bound,
  };
  tasks.pushTask(AfterLiteralHint{.draft = std::move(draft), .position = task.position});

  return tasks.pushTask(std::move(substitute));
}

void BoundCalls::afterLiteralHint(const AfterLiteralHint &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  const Type *hint = tasks.takeAnswer();
  size_t index = draft.literals[task.position];
  Expr **slot = &draft.call->args[index];
  draft.checked[index] = true;
  tasks.pushTask(NextLiteralArgument{.draft = std::move(draft), .position = task.position + 1});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = hint});
}

void BoundCalls::substituteBoundResult(GenericBindingDraft draft) {
  auto &tasks = this->state.walk.tasks;
  if (draft.result == nullptr) {
    return this->checkBoundArguments(std::move(draft), Type::error());
  }
  auto substitute = SubstituteType{
      .type = draft.result,
      .params = draft.generic->typeParams,
      .args = draft.bound,
  };
  tasks.pushTask(AfterBoundResult{.draft = std::move(draft)});

  return tasks.pushTask(std::move(substitute));
}

void BoundCalls::afterBoundResult(const AfterBoundResult &task) {
  const Type *result = this->state.walk.tasks.takeAnswer();

  return this->checkBoundArguments(task.draft, result);
}

void BoundCalls::checkBoundArguments(GenericBindingDraft draft, const Type *result) {
  auto &tasks = this->state.walk.tasks;
  CallExpr *call = draft.call;
  call->type = result;
  this->state.generics.copies.leaveInstanceLoc();
  size_t minArgs = draft.generic->minArgs;
  bool hasRest = draft.generic->hasRest;
  auto params = draft.params;
  auto flat = std::vector<const Type *>();
  if (ArgumentLists::flattenArguments(params, minArgs, hasRest, flat)) {
    params = std::move(flat);
  }
  auto checked = draft.typeArgs.empty() ? draft.checked : std::vector<bool>();
  auto arguments = CheckArguments{
      .call = call,
      .params = std::move(params),
      .minArgs = minArgs,
      .hasRest = hasRest,
      .loc = draft.loc,
      .checked = std::move(checked),
  };
  tasks.pushTask(AfterBoundArguments{.draft = std::move(draft)});

  return tasks.pushTask(std::move(arguments));
}

void BoundCalls::afterBoundArguments(const AfterBoundArguments &task) {
  this->state.walk.tasks.takeChecked();
  if (task.draft.result == nullptr) {
    return this->bindingRules.deliverFailure(task.draft);
  }

  return this->deliverBinding(task.draft);
}

void BoundCalls::deliverBinding(const GenericBindingDraft &draft) {
  switch (draft.owner) {
    case BindingOwner::BIND:
      return this->bindingRules.pushBound(draft.bound);
    case BindingOwner::CALL:
      return this->targetGenericCall(draft);
  }
}

void BoundCalls::targetGenericCall(const GenericBindingDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  CallExpr *call = draft.call;
  FuncSymbol *generic = draft.generic;
  call->target = FunctionCall{.func = generic};
  tasks.pushChecked(call);
  bool isCopyingReturn = this->state.code.late.isTemplate(generic) && !generic->hasReturnType();
  bool isInstantiable = this->isConcreteCall(generic, draft.bound) &&
                        !this->state.walk.functions.isInsideTrial() && !generic->inferring &&
                        (generic->hasReturnType() || isCopyingReturn);
  if (!isInstantiable) {
    return;
  }
  auto exact = this->exactClassArgs(call, generic);

  return tasks.pushTask(InstantiateCallTarget{
      .call = call,
      .generic = generic,
      .typeArgs = draft.bound,
      .loc = draft.loc,
      .exact = std::move(exact),
      .isCopyingReturn = isCopyingReturn,
  });
}

bool BoundCalls::isConcreteCall(const FuncSymbol *generic,
                                llvm::ArrayRef<const Type *> bound) const {
  for (const Type *type : bound) {
    if (TypeQueries::mentionsParam(type)) {
      return false;
    }
  }
  const ClassDecl *classDecl = generic->decl != nullptr ? generic->decl->cls() : nullptr;

  return classDecl == nullptr || generic->decl->isStatic() || classDecl->isEmitted();
}

std::vector<const Type *> BoundCalls::exactClassArgs(CallExpr *call, const FuncSymbol *generic) {
  auto exact = std::vector<const Type *>();
  if (generic->hasRest) {
    return exact;
  }
  size_t shared = std::min(generic->params.size(), call->args.size());
  for (size_t index = 0; index < shared; ++index) {
    if (this->isGenericClassParam(generic, generic->params[index])) {
      this->takeExactClass(call, index, generic->params.size(), exact);
    }
  }

  return exact;
}

bool BoundCalls::isGenericClassParam(const FuncSymbol *generic, const Type *param) const {
  if (param == nullptr || !param->isConstructor()) {
    return false;
  }

  return GenericCheckHelpers::mentionsAnyOf(param, generic->typeParams);
}

void BoundCalls::takeExactClass(CallExpr *call, size_t index, size_t paramCount,
                                std::vector<const Type *> &exact) {
  Expr *arg = ExprParts::withoutImplicitCasts(call->args[index]);
  const ClassDecl *classDecl = arg->type->classRef();
  if (classDecl == nullptr || classDecl->type == nullptr) {
    return;
  }
  auto dropped = this->implicitCasts(call->args[index]);
  exact.resize(paramCount, nullptr);
  exact[index] = arg->type;
  call->args[index] = arg;
  for (const CastExpr *conversion : dropped) {
    this->state.library.reflection.forgetConstructorCast(conversion);
  }
}

std::vector<const CastExpr *> BoundCalls::implicitCasts(const Expr *arg) const {
  auto casts = std::vector<const CastExpr *>();
  auto *conversion = dyn_cast<CastExpr>(arg);
  while (conversion != nullptr && conversion->isImplicit()) {
    casts.push_back(conversion);
    conversion = dyn_cast<CastExpr>(conversion->operand);
  }

  return casts;
}

}
