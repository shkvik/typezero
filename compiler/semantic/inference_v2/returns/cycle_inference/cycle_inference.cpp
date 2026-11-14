#include "compiler/semantic/inference_v2/returns/cycle_inference/cycle_inference.h"

namespace tz {

namespace {

constexpr int kMaxCycleRounds = 8;

}

CycleInference::CycleInference(SemanticState &owner, ReturnRules &sharedReturnRules)
    : state(owner),
      returnRules(sharedReturnRules) {}

void CycleInference::inferCycle(const InferCycle &task) {
  auto &returns = this->state.inference.returns;
  for (FuncSymbol *function : task.cycle) {
    function->inferring = true;
  }
  for (FuncSymbol *function : task.cycle) {
    returns.enterInference(function);
  }
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  auto draft = CycleDraft{
      .cycle = task.cycle,
      .round = 0,
      .index = 0,
      .roundStep = CycleStep::KEPT,
  };

  return this->nextCycleTrial(NextCycleTrial{.draft = std::move(draft)});
}

void CycleInference::nextCycleTrial(const NextCycleTrial &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (draft.index < draft.cycle.size()) {
    FuncSymbol *function = draft.cycle[draft.index];
    tasks.pushTask(AfterCycleTrial{.draft = std::move(draft)});

    return this->startTrialReturn(function);
  }
  if (draft.roundStep != CycleStep::CHANGED) {
    return this->finishCycle(draft, draft.roundStep);
  }
  draft.round += 1;
  if (draft.round == kMaxCycleRounds) {
    return this->finishCycle(draft, CycleStep::CHANGED);
  }
  draft.index = 0;
  draft.roundStep = CycleStep::KEPT;

  return tasks.pushTask(NextCycleTrial{.draft = std::move(draft)});
}

void CycleInference::afterCycleTrial(const AfterCycleTrial &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  const Type *found = tasks.takeAnswer();
  auto step = this->nextAssumption(draft.cycle[draft.index], found);
  if (step == CycleStep::FAILED) {
    return this->finishCycle(draft, step);
  }
  if (step == CycleStep::CHANGED) {
    draft.roundStep = step;
  }
  draft.index += 1;

  return tasks.pushTask(NextCycleTrial{.draft = std::move(draft)});
}

CycleStep CycleInference::nextAssumption(FuncSymbol *function, const Type *found) {
  if (found == nullptr) {
    return CycleStep::KEPT;
  }
  const Type *assumed = function->assumed;
  const Type *next =
      assumed != nullptr ? this->state.types.literals.unionUnify(assumed, found) : found;
  if (next == nullptr || next->isError()) {
    return CycleStep::FAILED;
  }
  if (next == assumed) {
    return CycleStep::KEPT;
  }
  function->assumed = next;

  return CycleStep::CHANGED;
}

void CycleInference::finishCycle(const CycleDraft &draft, CycleStep settled) {
  auto &returns = this->state.inference.returns;
  this->state.diagnostics.popList();
  for (size_t index = 0; index < draft.cycle.size(); ++index) {
    returns.leaveInference();
  }
  bool isFailed = settled == CycleStep::FAILED;
  for (FuncSymbol *function : draft.cycle) {
    function->inferring = false;
    bool isAssumed = function->assumed != nullptr && !isFailed;
    function->setReturnType(isAssumed ? function->assumed : Type::error());
    function->assumed = nullptr;
  }

  return this->walkCycleTypes(draft.cycle, isFailed);
}

void CycleInference::walkCycleTypes(llvm::ArrayRef<FuncSymbol *> cycle, bool isFailed) {
  int cycleSize = int(cycle.size());
  for (FuncSymbol *function : cycle) {
    auto known = this->cycleType(function, cycleSize, isFailed);
    if (known.isErr()) {
      this->state.diagnostics.error(function->loc, known.error().message);
    }
  }
}

CycleInference::ResultCycleTypeKnown CycleInference::cycleType(const FuncSymbol *function,
                                                               int cycleSize, bool isFailed) const {
  bool isUnknown = function->getKnownReturnType()->isError();
  if (isUnknown && isFailed) {
    auto message = ErrorTextCycleType::differ(function->name);

    return err(InferenceReturnsRegistryCycleTypeError{
        .code = InferenceReturnsRegistryCycleTypeErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }
  if (isUnknown && cycleSize == 1) {
    auto message = ErrorTextCycleType::selfOnly(function->name);

    return err(InferenceReturnsRegistryCycleTypeError{
        .code = InferenceReturnsRegistryCycleTypeErrorCode::ONLY_SELF_CALLS,
        .message = message,
    });
  }
  if (isUnknown) {
    auto message = ErrorTextCycleType::calledBack(function->name);

    return err(InferenceReturnsRegistryCycleTypeError{
        .code = InferenceReturnsRegistryCycleTypeErrorCode::ONLY_CALLED_BACK,
        .message = message,
    });
  }

  return ok(function->getKnownReturnType());
}

void CycleInference::startTrialReturn(FuncSymbol *function) {
  auto &tasks = this->state.walk.tasks;
  auto &functions = this->state.walk.functions;
  auto *copy = this->returnRules.freshCopy(function->decl);
  CollectedReturns &collected = this->state.inference.returns.openCollection();
  functions.rememberReturnCollector(&collected.returns);
  functions.rememberYieldCollector(&collected.yields);
  functions.enterTrial();
  tasks.pushTask(FinishTrialReturn{.function = function, .copy = copy});

  return tasks.pushTask(WalkFunctionFromScratch{.functionDecl = copy});
}

void CycleInference::finishTrialReturn(const FinishTrialReturn &task) {
  FuncSymbol *function = task.function;
  auto &functions = this->state.walk.functions;
  functions.leaveTrial();
  auto collected = this->state.inference.returns.takeCollection();
  functions.rememberReturnCollector(nullptr);
  functions.rememberYieldCollector(nullptr);
  bool isFallingOff = !this->state.flow.reachability.isAlwaysReturning(task.copy->body);
  const auto *type = this->returnRules.collectedType(function->decl, function, collected,
                                                     isFallingOff, ReturnErrors::IGNORED);

  return this->state.walk.tasks.pushAnswer(type);
}

}
