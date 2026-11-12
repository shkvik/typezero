#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/StringExtras.h"
#include <functional>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int kThisPosition = -1;
constexpr int kNoPosition = -2;
constexpr int kFirstParam = 0;
constexpr uint64_t kMaxTermLiteral = uint64_t(1) << 31;

}

PredicateChecker::PredicateChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->flowChecker = &all.flowChecker;
  this->literalTypes = &all.literalTypes;
  this->typeResolver = &all.typeResolver;
  this->returnInferrer = &all.returnInferrer;
  this->unionNarrowing = &all.unionNarrowing;
  this->functionChecker = &all.functionChecker;
}

const VarSymbol *PredicateChecker::argumentVariable(const Expr *expr) const {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    if (!conversion->isImplicit() || conversion->shortCircuit) {
      return nullptr;
    }
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }
  auto *name = dyn_cast<NameExpr>(inner);

  return name != nullptr ? name->var() : nullptr;
}

const Expr *PredicateChecker::withoutImplicitCasts(const Expr *expr) const {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    if (!conversion->isImplicit()) {
      return nullptr;
    }
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

std::optional<Linear> PredicateChecker::argumentTerm(const Expr *expr) const {
  const auto *inner = this->withoutImplicitCasts(expr);
  if (inner == nullptr) {
    return std::nullopt;
  }
  auto *unary = dyn_cast<UnaryExpr>(inner);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *value = isNegated ? unary->operand : inner;
  auto *literal = dyn_cast<IntLitExpr>(value);
  auto *name = dyn_cast<NameExpr>(value);
  if (literal != nullptr && literal->value < kMaxTermLiteral) {
    auto magnitude = int64_t(literal->value);

    return Linear{Term::zero(), isNegated ? -magnitude : magnitude};
  }
  if (name != nullptr && name->var() != nullptr && !isNegated) {
    return Linear{Term::of(name->var()), 0};
  }

  return std::nullopt;
}

PredicateChecker::ResultNotAsserting
PredicateChecker::notAsserting(const FunctionDecl *functionDecl, const TypeRef &ref) {
  if (!ref.asserts()) {
    return ok(&ref);
  }

  const auto *type = this->predicateType(ref);
  auto call = this->callText(functionDecl);
  auto shown = this->shownPredicate(ref, type);
  auto message = ErrorTextAsserts::notAllowed(shown, type != nullptr, call);

  return err(PredicateCheckerNotAssertingError{
      .code = PredicateCheckerNotAssertingErrorCode::ASSERTS_PREDICATE,
      .message = message,
  });
}

const Type *PredicateChecker::predicateType(const TypeRef &ref) const {
  if (ref.args.empty()) {
    return nullptr;
  }

  return this->typeResolver->resolveType(ref.args[0]);
}

std::string PredicateChecker::callText(const FunctionDecl *functionDecl) const {
  auto names = std::vector<llvm::StringRef>();
  for (const Param &param : functionDecl->params) {
    names.push_back(param.name);
  }
  auto args = llvm::join(names, ", ");
  auto call = functionDecl->name.str() + "(" + args + ")";
  if (!functionDecl->takesThis()) {
    return call;
  }
  auto object = this->receiverText(functionDecl);

  return object + "." + call;
}

std::string PredicateChecker::receiverText(const FunctionDecl *functionDecl) const {
  auto object = functionDecl->cls()->name.str();
  object[0] = llvm::toLower(object[0]);

  return object;
}

std::string PredicateChecker::shownPredicate(const TypeRef &ref, const Type *type) const {
  if (type == nullptr) {
    return ref.name.str();
  }

  return ref.name.str() + " is " + type->name().str();
}

PredicateChecker::ResultPredicateArg
PredicateChecker::predicateArg(const FunctionDecl *functionDecl, const TypeRef &ref) const {
  bool isThis = ref.name == KeywordTable::getName(Keyword::THIS);
  bool isMethod =
      functionDecl->cls() != nullptr && !functionDecl->isStatic() && !functionDecl->isConstructor();
  if (isThis && !isMethod) {
    auto message = ErrorTextPredicateArg::thisOutside();

    return err(PredicateCheckerPredicateArgError{
        .code = PredicateCheckerPredicateArgErrorCode::THIS_OUTSIDE_METHOD,
        .message = message,
    });
  }
  if (isThis) {
    return ok(kThisPosition);
  }

  auto param = this->lastParamNamed(functionDecl, ref.name);
  if (!param.has_value()) {
    auto message = ErrorTextPredicateArg::notParam(ref.name);

    return err(PredicateCheckerPredicateArgError{
        .code = PredicateCheckerPredicateArgErrorCode::NOT_A_PARAMETER,
        .message = message,
    });
  }

  return ok(*param);
}

std::optional<int> PredicateChecker::lastParamNamed(const FunctionDecl *functionDecl,
                                                    llvm::StringRef name) const {
  auto found = std::optional<int>();
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    if (functionDecl->params[index].name == name) {
      found = int(index);
    }
  }

  return found;
}

const Type *PredicateChecker::resolveReturnType(FunctionDecl *functionDecl, FuncSymbol *function) {
  const TypeRef &ref = *functionDecl->returnType;
  if (ref.kind() != TypeRefKind::PREDICATE_REF) {
    return this->typeResolver->resolveTypeOrVoid(ref);
  }
  bool isReporting = function != nullptr;
  if (isReporting) {
    this->reportAsserting(functionDecl, ref);
  }
  if (ref.asserts()) {
    return Type::voidTy();
  }
  auto named = this->predicateArg(functionDecl, ref);
  if (named.isErr() && isReporting) {
    this->state.getDiagnostics().error(ref.loc, named.error().message);
  }
  const auto *type = this->predicateType(ref);
  bool isRecorded = isReporting && named.isOk() && type != nullptr && !type->isError();
  if (isRecorded) {
    function->predicate = TypePredicate{.param = named.value(), .type = type};
  }

  return Type::boolTy();
}

void PredicateChecker::reportAsserting(const FunctionDecl *functionDecl, const TypeRef &ref) {
  auto plain = this->notAsserting(functionDecl, ref);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(ref.loc, plain.error().message);
  }
}

bool PredicateChecker::isLookingBoolean(const Expr *expr) const {
  for (const Expr *part : ExprParts::logicParts(expr)) {
    if (!this->isLookingLikeTest(part)) {
      return false;
    }
  }

  return true;
}

bool PredicateChecker::isLookingLikeTest(const Expr *expr) const {
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (isa<BoolLitExpr>(expr)) {
    return true;
  }
  if (unary != nullptr) {
    return unary->op == UnaryOp::Not;
  }
  if (binary != nullptr) {
    return this->isTestOperator(binary->op);
  }

  return false;
}

bool PredicateChecker::isTestOperator(BinaryOp op) const {
  bool isComparison = op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt ||
                      op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge;

  return isComparison || op == BinaryOp::InstanceOf || op == BinaryOp::In;
}

void PredicateChecker::beginPredicate(FunctionDecl *functionDecl) {
  const FuncSymbol *function = functionDecl->sym;
  bool isBoolean = !function->hasReturnType() || function->getKnownReturnType()->isBool();
  bool isPredicate = isBoolean && !functionDecl->isGenerator && !functionDecl->isConstructor();
  if (!isPredicate) {
    auto skipped = PredicateBuild{.fn = nullptr, .decl = functionDecl};
    this->state.getPredicateStore().enterPredicateBuild(std::move(skipped));
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
  this->state.getPredicateStore().enterPredicateBuild(std::move(build));
}

PredicateChecker::ResultExitNarrowed PredicateChecker::notePredicateExit(const Flow &whenTrue,
                                                                         const Flow &whenFalse) {
  auto &predicates = this->state.getPredicateStore();
  if (!predicates.isBuildingPredicate()) {
    return ok(&whenTrue);
  }
  const auto &build = predicates.getPredicateBuild();
  const FuncSymbol *symbol = build.fn;
  const FunctionDecl *functionDecl = build.decl;
  auto joinedTrue = this->flowChecker->join(build.whenTrue, whenTrue);
  auto joinedFalse = this->flowChecker->join(build.whenFalse, whenFalse);
  predicates.updatePredicateBuild(std::move(joinedTrue), std::move(joinedFalse));
  if (!symbol->predicate.has_value() || whenTrue.unreachable) {
    return ok(&whenTrue);
  }
  const TypePredicate &predicate = *symbol->predicate;
  const auto *variable = this->predicatedVariable(functionDecl, predicate.param);
  if (variable == nullptr) {
    return ok(&whenTrue);
  }
  const auto *shown = this->literalTypes->typeIn(whenTrue, variable);
  bool isNarrowed = shown->isError() || shown->isNever() ||
                    TypeRelations::isImplicitlyConvertible(shown, predicate.type);
  if (!isNarrowed) {
    auto function = ErrorTextFunctionName::label(symbol->name);
    auto expected = predicate.type->name();
    auto actual = shown->name();
    auto message = ErrorTextPredicateExit::mayLie(function, variable->name, expected, actual);

    return err(PredicateCheckerNotePredicateExitError{
        .code = PredicateCheckerNotePredicateExitErrorCode::NOT_NARROWED,
        .message = message,
    });
  }

  return ok(&whenTrue);
}

const VarSymbol *PredicateChecker::predicatedVariable(const FunctionDecl *functionDecl,
                                                      int param) const {
  if (param == kThisPosition) {
    return functionDecl->thisVar();
  }
  if (size_t(param) >= functionDecl->params.size()) {
    return nullptr;
  }

  return functionDecl->params[size_t(param)].sym;
}

void PredicateChecker::endPredicate(FunctionDecl *functionDecl) {
  PredicateBuild build = this->state.getPredicateStore().leavePredicateBuild();
  if (build.fn == nullptr) {
    return;
  }
  auto summary = Predicate{
      .whenTrue = this->projectedSide(functionDecl, build.whenTrue),
      .whenFalse = this->projectedSide(functionDecl, build.whenFalse),
      .params = this->paramSymbols(functionDecl),
      .self = functionDecl->thisVar(),
  };
  this->state.getPredicateStore().rememberPredicate(build.fn, std::move(summary));
}

std::vector<const VarSymbol *>
PredicateChecker::paramSymbols(const FunctionDecl *functionDecl) const {
  auto symbols = std::vector<const VarSymbol *>();
  for (const Param &param : functionDecl->params) {
    symbols.push_back(param.sym);
  }

  return symbols;
}

int PredicateChecker::paramPosition(const FunctionDecl *functionDecl,
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

PredicateSide PredicateChecker::projectedSide(const FunctionDecl *functionDecl,
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
PredicateChecker::projectedNarrowed(const FunctionDecl *functionDecl, const Flow &flow) const {
  auto narrowed = std::vector<std::pair<int, const Type *>>();
  for (const auto &[variable, type] : flow.narrowed) {
    int index = this->paramPosition(functionDecl, variable);
    if (index != kNoPosition) {
      narrowed.push_back({index, type});
    }
  }

  return narrowed;
}

std::vector<int> PredicateChecker::projectedDefined(const FunctionDecl *functionDecl,
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
PredicateChecker::projectedFields(const FunctionDecl *functionDecl, const Flow &flow) const {
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
PredicateChecker::projectedNarrowedFields(const FunctionDecl *functionDecl,
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

bool PredicateChecker::isParamTerm(const FunctionDecl *functionDecl, const Term &term) const {
  return term.isZero() ||
         (!term.isSum() && this->paramPosition(functionDecl, term.var) != kNoPosition);
}

std::vector<Facts::Fact> PredicateChecker::projectedFacts(const FunctionDecl *functionDecl,
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

const Predicate *PredicateChecker::predicateOf(const FuncSymbol *function) const {
  auto &predicates = this->state.getPredicateStore();
  const auto *known = predicates.findPredicate(function);
  if (known != nullptr) {
    return known;
  }
  FunctionDecl *functionDecl = function->decl;
  if (!this->isSummarizable(function, functionDecl)) {
    return nullptr;
  }
  if (function->inferring) {
    return this->predicateFromCopy(function);
  }
  this->returnInferrer->walkBodyNow(functionDecl);

  return predicates.findPredicate(function);
}

bool PredicateChecker::isSummarizable(const FuncSymbol *function,
                                      const FunctionDecl *functionDecl) const {
  if (functionDecl == nullptr || functionDecl->checked || functionDecl->body == nullptr) {
    return false;
  }
  if (functionDecl->external() || functionDecl->isGenerator || functionDecl->isInstance) {
    return false;
  }

  return !function->hasReturnType() || function->getKnownReturnType()->isBool();
}

const Predicate *PredicateChecker::predicateFromCopy(const FuncSymbol *function) const {
  auto &predicates = this->state.getPredicateStore();
  if (predicates.isSummarizing(function)) {
    return nullptr;
  }
  predicates.markSummarizing(function);
  FunctionDecl *functionDecl = function->decl;
  auto saved = this->returnInferrer->enterBody(functionDecl);
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  auto *copy = this->returnInferrer->freshCopy(functionDecl);
  this->functionChecker->walkFunctionBody(copy);
  this->state.getDiagnostics().popList();
  this->returnInferrer->leaveBody(saved);

  return predicates.findPredicate(function);
}

void PredicateChecker::applyPredicate(const CallExpr *call, Flow &whenTrue, Flow &whenFalse) const {
  const FuncSymbol *function = call->func() != nullptr ? call->func() : call->method();
  if (function == nullptr || call->virtualCall() || !call->unionMethods().empty()) {
    return;
  }
  auto applied = PredicateCall{
      .call = call,
      .member = dyn_cast<MemberExpr>(call->callee),
      .summary = this->predicateOf(function),
  };
  if (applied.summary != nullptr) {
    this->applySide(applied, applied.summary->whenTrue, whenTrue);
    this->applySide(applied, applied.summary->whenFalse, whenFalse);
  }
  if (!function->predicate.has_value()) {
    return;
  }
  const auto *variable = this->predicateVariable(applied, function->predicate->param);
  if (variable != nullptr) {
    this->narrowParamTo(whenTrue, variable, function->predicate->type);
  }
}

const Expr *PredicateChecker::predicateArgument(const PredicateCall &applied, int index) const {
  const CallExpr *call = applied.call;
  if (index == kThisPosition) {
    bool isMethodCall = call->method() != nullptr && applied.member != nullptr;

    return isMethodCall ? applied.member->object : nullptr;
  }
  if (size_t(index) >= call->args.size()) {
    return nullptr;
  }

  return call->args[size_t(index)];
}

const VarSymbol *PredicateChecker::predicateVariable(const PredicateCall &applied,
                                                     int index) const {
  const auto *arg = this->predicateArgument(applied, index);
  if (arg == nullptr) {
    return nullptr;
  }
  const auto *variable = this->argumentVariable(arg);
  bool isStable = variable != nullptr && !this->flowChecker->isChangedByCalls(variable);

  return isStable ? variable : nullptr;
}

void PredicateChecker::narrowParamTo(Flow &flow, const VarSymbol *variable,
                                     const Type *type) const {
  const auto *now = this->literalTypes->typeIn(flow, variable);
  if (TypeRelations::isImplicitlyConvertible(now, type)) {
    return;
  }
  if (TypeRelations::isImplicitlyConvertible(type, now)) {
    this->unionNarrowing->narrowVariable(flow, variable, type);
    return;
  }
  const Type *value = now->nonOptional();
  if (!value->isUnion()) {
    return;
  }
  auto kept = this->convertibleMembers(value, type);
  const Type *narrowed = kept.empty() ? nullptr : Type::unionOf(kept);
  this->unionNarrowing->narrowVariable(flow, variable, narrowed);
}

std::vector<const Type *> PredicateChecker::convertibleMembers(const Type *value,
                                                               const Type *type) const {
  auto kept = std::vector<const Type *>();
  for (const Type *member : value->members()) {
    if (TypeRelations::isImplicitlyConvertible(member, type)) {
      kept.push_back(member);
    }
  }

  return kept;
}

void PredicateChecker::applySide(const PredicateCall &applied, const PredicateSide &side,
                                 Flow &flow) const {
  if (side.unreachable) {
    flow.unreachable = true;
    return;
  }
  this->applyNarrowed(applied, side, flow);
  this->applyDefined(applied, side, flow);
  this->applyFields(applied, side, flow);
  this->applyNarrowedFields(applied, side, flow);
  this->applyFacts(applied, side, flow);
}

void PredicateChecker::applyNarrowed(const PredicateCall &applied, const PredicateSide &side,
                                     Flow &flow) const {
  for (auto [param, type] : side.narrowed) {
    const auto *variable = this->predicateVariable(applied, param);
    if (variable != nullptr) {
      this->narrowParamTo(flow, variable, type);
    }
  }
}

void PredicateChecker::applyDefined(const PredicateCall &applied, const PredicateSide &side,
                                    Flow &flow) const {
  for (int param : side.defined) {
    const auto *variable = this->predicateVariable(applied, param);
    if (variable != nullptr && variable->type->isOptional()) {
      flow.defined.insert(variable);
    }
  }
}

void PredicateChecker::applyFields(const PredicateCall &applied, const PredicateSide &side,
                                   Flow &flow) const {
  for (const auto &[param, names] : side.fields) {
    const auto *variable = this->predicateVariable(applied, param);
    if (variable != nullptr) {
      auto path = FieldPath{.var = variable, .names = names};
      this->fieldFacts->defineField(flow, path);
    }
  }
}

void PredicateChecker::applyNarrowedFields(const PredicateCall &applied, const PredicateSide &side,
                                           Flow &flow) const {
  for (const PredicateSide::NarrowedField &field : side.narrowedFields) {
    const auto *variable = this->predicateVariable(applied, field.param);
    if (variable != nullptr) {
      auto path = FieldPath{.var = variable, .names = field.names};
      this->narrowFieldTo(flow, path, field.type);
    }
  }
}

bool PredicateChecker::isPathEntry(const FieldPath &path,
                                   const std::pair<FieldPath, const Type *> &entry) const {
  return entry.first == path;
}

void PredicateChecker::narrowFieldTo(Flow &flow, const FieldPath &path, const Type *type) const {
  auto isEntry = std::bind_front(&PredicateChecker::isPathEntry, this, std::cref(path));
  llvm::erase_if(flow.narrowedFields, isEntry);
  flow.narrowedFields.push_back({path, type});
}

void PredicateChecker::applyFacts(const PredicateCall &applied, const PredicateSide &side,
                                  Flow &flow) const {
  for (const Facts::Fact &fact : side.facts) {
    auto left = this->factTerm(applied, fact.lhs);
    auto right = this->factTerm(applied, fact.rhs);
    if (left.has_value() && right.has_value()) {
      flow.facts.add(left->term, right->term, fact.offset + right->offset - left->offset);
    }
  }
}

int PredicateChecker::summaryParam(const Predicate *summary, const VarSymbol *variable) const {
  for (size_t index = 0; index < summary->params.size(); ++index) {
    if (summary->params[index] == variable) {
      return int(index);
    }
  }

  return kNoPosition;
}

std::optional<Linear> PredicateChecker::factTerm(const PredicateCall &applied,
                                                 const Term &term) const {
  if (term.isZero()) {
    return Linear{Term::zero(), 0};
  }
  int index = this->summaryParam(applied.summary, term.var);
  if (index == kNoPosition) {
    return std::nullopt;
  }
  const auto *arg = this->predicateArgument(applied, index);
  if (arg == nullptr) {
    return std::nullopt;
  }
  if (term.length) {
    return this->lengthTerm(applied, index);
  }
  auto value = this->argumentTerm(arg);
  bool hasChanged = value.has_value() && value->term.var != nullptr &&
                    this->flowChecker->isChangedByCalls(value->term.var);

  return hasChanged ? std::nullopt : value;
}

std::optional<Linear> PredicateChecker::lengthTerm(const PredicateCall &applied, int index) const {
  const auto *variable = this->predicateVariable(applied, index);
  if (variable == nullptr) {
    return std::nullopt;
  }

  return Linear{Term::lengthOf(variable), 0};
}

const Type *PredicateChecker::filteredType(const Expr *callback, const Type *element) const {
  const auto *function = this->callbackFunction(callback);
  if (function == nullptr) {
    return nullptr;
  }
  const auto *kept = this->keptByPredicate(function, element);
  bool isNarrowing =
      kept != nullptr && kept != element && TypeRelations::isImplicitlyConvertible(kept, element);

  return isNarrowing ? kept : nullptr;
}

const FuncSymbol *PredicateChecker::callbackFunction(const Expr *callback) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(callback);
  auto *name = dyn_cast<NameExpr>(callback);
  if (functionExpr != nullptr) {
    return functionExpr->fn->sym;
  }
  if (name != nullptr) {
    return this->namedFunction(name);
  }

  return nullptr;
}

const FuncSymbol *PredicateChecker::namedFunction(const NameExpr *name) const {
  if (name->func() != nullptr) {
    return name->func();
  }
  const VarSymbol *variable = name->var();
  if (variable != nullptr && variable->function() != nullptr) {
    return variable->function()->sym;
  }

  return nullptr;
}

const Type *PredicateChecker::keptByPredicate(const FuncSymbol *function,
                                              const Type *element) const {
  bool isOnFirstParam =
      function->predicate.has_value() && function->predicate->param == kFirstParam;
  const auto *summary = this->predicateOf(function);
  if (isOnFirstParam) {
    return function->predicate->type;
  }
  if (summary == nullptr) {
    return nullptr;
  }

  return this->keptBySide(summary->whenTrue, element);
}

const Type *PredicateChecker::keptBySide(const PredicateSide &side, const Type *element) const {
  if (side.unreachable) {
    return nullptr;
  }
  const auto *narrowed = this->firstParamNarrowed(side);
  if (narrowed != nullptr) {
    return narrowed;
  }
  bool isDefined = element->isOptional() && llvm::is_contained(side.defined, kFirstParam);

  return isDefined ? element->inner() : nullptr;
}

const Type *PredicateChecker::firstParamNarrowed(const PredicateSide &side) const {
  const Type *narrowed = nullptr;
  for (auto [param, type] : side.narrowed) {
    if (param == kFirstParam) {
      narrowed = type;
    }
  }

  return narrowed;
}

}
