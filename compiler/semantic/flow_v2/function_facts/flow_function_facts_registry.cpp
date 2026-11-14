#include "compiler/semantic/flow_v2/function_facts/flow_function_facts_registry.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

namespace {

constexpr int kThisPosition = -1;
constexpr int kNoPosition = -2;

}

FlowFunctionFactsRegistry::FlowFunctionFactsRegistry(SemanticState &owner)
    : state(owner) {}

void FlowFunctionFactsRegistry::beginPredicate(const BeginPredicate &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  const FuncSymbol *function = functionDecl->sym;
  bool isBoolean = !function->hasReturnType() || function->getKnownReturnType()->isBool();
  bool isPredicate = isBoolean && !functionDecl->isGenerator && !functionDecl->isConstructor();
  if (!isPredicate) {
    auto skipped = PredicateBuild{.fn = nullptr, .decl = functionDecl};
    this->state.flow.predicates.enterPredicateBuild(std::move(skipped));
    return;
  }
  auto impossible = Flow();
  impossible.unreachable = true;
  auto build = PredicateBuild{
      .fn = function,
      .decl = functionDecl,
      .whenTrue = impossible,
      .whenFalse = impossible,
  };
  this->state.flow.predicates.enterPredicateBuild(std::move(build));
}

void FlowFunctionFactsRegistry::notePredicateExit(const NotePredicateExit &task) {
  auto narrowed = this->checkPredicateExit(task.whenTrue, task.whenFalse);
  if (narrowed.isErr()) {
    return this->state.diagnostics.error(task.value->loc(), narrowed.error().message);
  }
}

FlowFunctionFactsRegistry::ResultExitNarrowed
FlowFunctionFactsRegistry::checkPredicateExit(const Flow &whenTrue, const Flow &whenFalse) {
  auto &predicates = this->state.flow.predicates;
  if (!predicates.isBuildingPredicate()) {
    return ok(&whenTrue);
  }
  const auto &build = predicates.getPredicateBuild();
  const FuncSymbol *symbol = build.fn;
  const FunctionDecl *functionDecl = build.decl;
  auto joinedTrue = this->state.flow.joins.join(build.whenTrue, whenTrue);
  auto joinedFalse = this->state.flow.joins.join(build.whenFalse, whenFalse);
  predicates.updatePredicateBuild(std::move(joinedTrue), std::move(joinedFalse));
  if (!symbol->predicate.has_value() || whenTrue.unreachable) {
    return ok(&whenTrue);
  }
  const TypePredicate &predicate = *symbol->predicate;
  const auto *variable = this->predicatedVariable(functionDecl, predicate.param);
  if (variable == nullptr) {
    return ok(&whenTrue);
  }
  const auto *shown = this->state.flow.tracked.typeIn(whenTrue, variable);
  bool isNarrowed = shown->isError() || shown->isNever() ||
                    this->state.types.relations.isImplicitlyConvertible(shown, predicate.type);
  if (!isNarrowed) {
    auto function = ErrorTextFunctionName::label(symbol->name);
    auto expected = predicate.type->name();
    auto actual = shown->name();
    auto message = ErrorTextPredicateExit::mayLie(function, variable->name, expected, actual);

    return err(FlowFunctionFactsRegistryNotePredicateExitError{
        .code = FlowFunctionFactsRegistryNotePredicateExitErrorCode::NOT_NARROWED,
        .message = message,
    });
  }

  return ok(&whenTrue);
}

const VarSymbol *FlowFunctionFactsRegistry::predicatedVariable(const FunctionDecl *functionDecl,
                                                               int param) const {
  if (param == kThisPosition) {
    return functionDecl->thisVar();
  }
  if (size_t(param) >= functionDecl->params.size()) {
    return nullptr;
  }

  return functionDecl->params[size_t(param)].sym;
}

void FlowFunctionFactsRegistry::endPredicate(const EndPredicate &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  PredicateBuild build = this->state.flow.predicates.leavePredicateBuild();
  if (build.fn == nullptr) {
    return;
  }
  auto summary = Predicate{
      .whenTrue = this->projectedSide(functionDecl, build.whenTrue),
      .whenFalse = this->projectedSide(functionDecl, build.whenFalse),
      .params = this->paramSymbols(functionDecl),
      .self = functionDecl->thisVar(),
  };
  this->state.flow.predicates.rememberPredicate(build.fn, std::move(summary));
}

std::vector<const VarSymbol *>
FlowFunctionFactsRegistry::paramSymbols(const FunctionDecl *functionDecl) const {
  auto symbols = std::vector<const VarSymbol *>();
  for (const Param &param : functionDecl->params) {
    symbols.push_back(param.sym);
  }

  return symbols;
}

int FlowFunctionFactsRegistry::paramPosition(const FunctionDecl *functionDecl,
                                             const VarSymbol *variable) const {
  if (variable == nullptr || variable->reassigned) {
    return kNoPosition;
  }
  if (variable == functionDecl->thisVar()) {
    return kThisPosition;
  }
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    if (functionDecl->params[index].sym == variable) {
      return int(index);
    }
  }

  return kNoPosition;
}

PredicateSide FlowFunctionFactsRegistry::projectedSide(const FunctionDecl *functionDecl,
                                                       const Flow &flow) const {
  if (flow.unreachable) {
    return PredicateSide{.unreachable = true};
  }

  return PredicateSide{
      .unreachable = false,
      .narrowed = this->projectedNarrowed(functionDecl, flow),
      .defined = this->projectedDefined(functionDecl, flow),
      .fields = this->projectedFields(functionDecl, flow),
      .narrowedFields = this->projectedNarrowedFields(functionDecl, flow),
      .facts = this->projectedFacts(functionDecl, flow),
  };
}

std::vector<std::pair<int, const Type *>>
FlowFunctionFactsRegistry::projectedNarrowed(const FunctionDecl *functionDecl,
                                             const Flow &flow) const {
  auto narrowed = std::vector<std::pair<int, const Type *>>();
  for (const auto &[variable, type] : flow.narrowed) {
    int index = this->paramPosition(functionDecl, variable);
    if (index != kNoPosition) {
      narrowed.push_back({index, type});
    }
  }

  return narrowed;
}

std::vector<int> FlowFunctionFactsRegistry::projectedDefined(const FunctionDecl *functionDecl,
                                                             const Flow &flow) const {
  auto defined = std::vector<int>();
  for (const VarSymbol *variable : flow.defined) {
    int index = this->paramPosition(functionDecl, variable);
    if (index != kNoPosition) {
      defined.push_back(index);
    }
  }

  return defined;
}

std::vector<std::pair<int, llvm::SmallVector<llvm::StringRef, 2>>>
FlowFunctionFactsRegistry::projectedFields(const FunctionDecl *functionDecl,
                                           const Flow &flow) const {
  auto fields = std::vector<std::pair<int, llvm::SmallVector<llvm::StringRef, 2>>>();
  for (const FieldPath &path : flow.definedFields) {
    int index = this->paramPosition(functionDecl, path.var);
    if (index != kNoPosition) {
      fields.push_back({index, path.names});
    }
  }

  return fields;
}

std::vector<PredicateSide::NarrowedField>
FlowFunctionFactsRegistry::projectedNarrowedFields(const FunctionDecl *functionDecl,
                                                   const Flow &flow) const {
  auto fields = std::vector<PredicateSide::NarrowedField>();
  for (const auto &[path, type] : flow.narrowedFields) {
    int index = this->paramPosition(functionDecl, path.var);
    if (index != kNoPosition) {
      fields.push_back({.param = index, .names = path.names, .type = type});
    }
  }

  return fields;
}

bool FlowFunctionFactsRegistry::isParamTerm(const FunctionDecl *functionDecl,
                                            const Term &term) const {
  return term.isZero() ||
         (!term.isSum() && this->paramPosition(functionDecl, term.var) != kNoPosition);
}

std::vector<Facts::Fact> FlowFunctionFactsRegistry::projectedFacts(const FunctionDecl *functionDecl,
                                                                   const Flow &flow) const {
  auto facts = std::vector<Facts::Fact>();
  for (const Facts::Fact &fact : flow.facts.all()) {
    bool hasParams =
        this->isParamTerm(functionDecl, fact.lhs) && this->isParamTerm(functionDecl, fact.rhs);
    if (hasParams) {
      facts.push_back(fact);
    }
  }

  return facts;
}

void FlowFunctionFactsRegistry::buildPredicate(const BuildPredicate &task) {
  const FuncSymbol *function = task.function;
  if (!this->state.flow.functions.isPredicateNeeded(function)) {
    return;
  }
  FunctionDecl *functionDecl = function->decl;
  auto &tasks = this->state.walk.tasks;
  if (!function->inferring) {
    return tasks.pushTask(WalkBodyNow{.functionDecl = functionDecl});
  }
  this->state.flow.predicates.markSummarizing(function);
  tasks.pushTask(LeaveReturnBody{});
  tasks.pushTask(DropDiagnosticList{});
  tasks.pushTask(StartPredicateCopy{.functionDecl = functionDecl});

  return tasks.pushTask(EnterReturnBody{.functionDecl = functionDecl});
}

void FlowFunctionFactsRegistry::startPredicateCopy(const StartPredicateCopy &task) {
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);

  return this->state.walk.tasks.pushTask(WalkFreshCopy{.functionDecl = task.functionDecl});
}

void FlowFunctionFactsRegistry::addIndexCallbackFacts(const AddIndexCallbackFacts &task) {
  this->addElementFacts(task.functionDecl);
  this->addIndexParamFacts(task.functionDecl);
}

void FlowFunctionFactsRegistry::addElementFacts(const FunctionDecl *functionDecl) {
  const auto *element = this->state.flow.callbacks.findElementCallback(functionDecl);
  if (element == nullptr) {
    return;
  }
  const VarSymbol *value = this->paramVariable(functionDecl, element->param);
  if (value == nullptr || !value->type->isInt()) {
    return;
  }
  auto &facts = this->state.walk.flow.getFacts();
  int64_t below = Facts::saturatingSub(1, element->lo);
  int64_t above = Facts::saturatingAdd(element->hi, 1);
  facts.add(Term::zero(), Term::of(value), below);
  facts.add(Term::of(value), Term::zero(), above);
}

void FlowFunctionFactsRegistry::addIndexParamFacts(const FunctionDecl *functionDecl) {
  auto index = this->state.flow.callbacks.findIndexParam(functionDecl);
  if (!index.has_value()) {
    return;
  }
  const VarSymbol *variable = this->paramVariable(functionDecl, *index);
  if (variable == nullptr || !variable->type->isInt()) {
    return;
  }
  auto &facts = this->state.walk.flow.getFacts();
  facts.add(Term::zero(), Term::of(variable), 1);
  facts.add(Term::of(variable), Term::zero(), Type::kMaxLength);
}

const VarSymbol *FlowFunctionFactsRegistry::paramVariable(const FunctionDecl *functionDecl,
                                                          size_t index) const {
  if (index >= functionDecl->params.size()) {
    return nullptr;
  }

  return functionDecl->params[index].sym;
}

void FlowFunctionFactsRegistry::addByteCallbackFacts(const AddByteCallbackFacts &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  const auto *found = this->state.flow.callbacks.findByteCallback(functionDecl);
  if (found == nullptr || found->param >= functionDecl->params.size()) {
    return;
  }
  const ByteCallback &info = *found;
  const VarSymbol *variable = functionDecl->params[info.param].sym;
  if (variable == nullptr || !variable->type->isInt() ||
      (info.lo <= kIntMin && info.hi >= kIntMax)) {
    return;
  }
  this->state.walk.flow.getFacts().add(Term::zero(), Term::of(variable),
                                       Facts::saturatingSub(1, info.lo));
  this->state.walk.flow.getFacts().add(Term::of(variable), Term::zero(),
                                       Facts::saturatingAdd(info.hi, 1));
}

void FlowFunctionFactsRegistry::checkByteCallbackResult(const CheckByteCallbackResult &task) {
  const auto *found = this->state.flow.callbacks.findByteCallback(task.functionDecl);
  if (found == nullptr || found->target == nullptr) {
    return;
  }
  auto what = ErrorTextByteView::element(found->target->name());
  auto fitting = this->state.flow.proofs.checkValueFits(task.value, found->target, what);
  if (fitting.isErr()) {
    return this->state.diagnostics.error(task.value->loc(), fitting.error().message);
  }
}

}
