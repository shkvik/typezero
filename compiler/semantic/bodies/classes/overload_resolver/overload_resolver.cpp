#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int kNoRank = -1;
constexpr int kExactRank = 0;
constexpr int kDirectRank = 1;
constexpr int kFieldsRank = 2;
constexpr int kJsonRank = 3;
constexpr llvm::StringLiteral kMatchIndices = "indices";

}

OverloadResolver::OverloadResolver(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeParams = &all.typeParams;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeRelations = &all.typeRelations;
  this->returnInferrer = &all.returnInferrer;
  this->decimalChecker = &all.decimalChecker;
  this->argumentChecker = &all.argumentChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->genericInstantiator = &all.genericInstantiator;
}

int OverloadResolver::conversionRank(const Type *from, const Type *to) const {
  auto run = RankRun();
  run.steps.push_back(RankGoal{.from = from, .to = to});
  while (!run.steps.empty()) {
    RankStep step = std::move(run.steps.back());
    run.steps.pop_back();
    switch (step.kind()) {
      case RankStepKind::GOAL:
        this->enterRank(*step.as<RankGoal>(), run);
        break;
      case RankStepKind::JOIN:
        this->joinRanks(*step.as<RankJoin>(), run);
        break;
    }
  }

  return run.answers.back();
}

void OverloadResolver::enterRank(const RankGoal &goal, RankRun &run) const {
  auto pair = std::pair<const Type *, const Type *>{goal.from, goal.to};
  if (llvm::is_contained(run.path, pair)) {
    run.answers.push_back(kNoRank);
    return;
  }
  auto rank = this->rankLeaf(goal.from, goal.to);
  if (rank.has_value()) {
    run.answers.push_back(*rank);
    return;
  }
  auto join = this->rankJoin(goal.from, goal.to);
  auto parts = join.parts;
  run.path.push_back(pair);
  run.steps.push_back(std::move(join));
  for (const RankGoal &part : llvm::reverse(parts)) {
    run.steps.push_back(part);
  }
}

std::optional<int> OverloadResolver::rankLeaf(const Type *from, const Type *to) const {
  if (from == to || from->isError() || to->isError()) {
    return kExactRank;
  }
  if (to->isOptional() && from->isUndefined()) {
    return kDirectRank;
  }
  if (to->isOptional()) {
    return std::nullopt;
  }
  if (from->isNever()) {
    return kDirectRank;
  }
  if (to->isUnion()) {
    return std::nullopt;
  }
  if (to->isJson()) {
    return this->jsonRank(from);
  }
  if (from->isParam() && from->constraint() != nullptr) {
    return std::nullopt;
  }
  if (from->isIterator() && to->isIterator()) {
    return this->iteratorRank(from, to);
  }
  if (from->isBytes() || to->isBytes()) {
    return this->bytesRank(from, to);
  }
  if (this->isOpaqueRanked(from) || this->isOpaqueRanked(to)) {
    return this->matchRank(from, to);
  }

  return this->objectRank(from, to);
}

int OverloadResolver::jsonRank(const Type *from) const {
  bool isJsonArray = from->isArray() && from->inner()->isJson();
  bool isJsonValue = from->isString() || from->isNumeric() || from->isBool() || isJsonArray;
  if (!isJsonValue) {
    return kNoRank;
  }

  return kJsonRank;
}

int OverloadResolver::iteratorRank(const Type *from, const Type *to) const {
  bool isSameReturn = from->returned() == to->returned() || to->returned()->isVoid();
  bool isSameSent = from->sent() == to->sent() || to->sent()->isVoid();
  bool isSame = from->yielded() == to->yielded() && isSameReturn && isSameSent;
  if (!isSame) {
    return kNoRank;
  }

  return kDirectRank;
}

int OverloadResolver::bytesRank(const Type *from, const Type *to) const {
  bool isBufferView = from->isBuffer() && to->bytes() == Type::Bytes::Uint8;
  if (!isBufferView) {
    return kNoRank;
  }

  return kDirectRank;
}

int OverloadResolver::matchRank(const Type *from, const Type *to) const {
  bool isPlainMatch = from->isMatch() && to->isMatch() && to->matchGroups() < 0 &&
                      from->field(kMatchIndices) == nullptr;
  if (!isPlainMatch) {
    return kNoRank;
  }

  return kDirectRank;
}

bool OverloadResolver::isOpaqueRanked(const Type *type) const {
  return type->isMatch() || type->isRegExp() || type->isDate() || type->isIntl() ||
         type->isCrypto() || type->isWeakRef();
}

std::optional<int> OverloadResolver::objectRank(const Type *from, const Type *to) const {
  int distance = this->subtypeDistance(from, to);
  if (distance > 0) {
    return distance;
  }
  if (this->hasSameFields(from, to)) {
    return kDirectRank;
  }
  if (this->isSameShape(from, to)) {
    return std::nullopt;
  }
  if (from->isResult() && to->isResult()) {
    return std::nullopt;
  }

  return kNoRank;
}

int OverloadResolver::subtypeDistance(const Type *from, const Type *to) const {
  if (!from->isObject() || !to->isObject()) {
    return 0;
  }

  return TypeQueries::subtypeDistance(from, to);
}

bool OverloadResolver::hasSameFields(const Type *from, const Type *to) const {
  bool isPlainPair = from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
                     !from->isAnyObject() && !to->isAnyObject();
  bool isSameLayout = from->isTuple() == to->isTuple() &&
                      from->fields().size() == to->fields().size() && from->overloads().empty() &&
                      to->overloads().empty();
  if (!isPlainPair || !isSameLayout) {
    return false;
  }
  for (const Field &field : from->fields()) {
    const Field *same = to->field(field.name);
    bool isSame = same != nullptr && same->type == field.type;
    bool isLosingReadonly = same != nullptr && field.readonly && !same->readonly;
    if (!isSame || isLosingReadonly) {
      return false;
    }
  }

  return true;
}

bool OverloadResolver::isShapeObject(const Type *type) const {
  return type->isObject() && !type->isClass() && !type->isAnyObject() && !type->isTuple() &&
         type->overloads().empty();
}

bool OverloadResolver::isSameShape(const Type *from, const Type *to) const {
  if (!this->isShapeObject(from) || !this->isShapeObject(to)) {
    return false;
  }
  for (const Field &field : from->fields()) {
    if (to->field(field.name) == nullptr) {
      return false;
    }
  }
  for (const Field &wanted : to->fields()) {
    bool isOmittable = wanted.type->isOptional() || wanted.type->isUndefined();
    if (from->field(wanted.name) == nullptr && !isOmittable) {
      return false;
    }
  }

  return true;
}

RankJoin OverloadResolver::rankJoin(const Type *from, const Type *to) const {
  if (to->isOptional()) {
    return this->optionalJoin(from, to);
  }
  if (to->isUnion() && from->isUnion()) {
    return this->membersJoin(from, to);
  }
  if (to->isUnion()) {
    return this->targetsJoin(from, to);
  }
  if (from->isParam() && from->constraint() != nullptr) {
    auto constraint = RankGoal{.from = from->constraint(), .to = to};

    return RankJoin{.from = from, .to = to, .form = RankForm::SAME, .parts = {constraint}};
  }
  if (from->isResult() && to->isResult()) {
    return this->resultJoin(from, to);
  }

  return this->fieldsJoin(from, to);
}

RankJoin OverloadResolver::optionalJoin(const Type *from, const Type *to) const {
  if (from->isOptional()) {
    auto inner = RankGoal{.from = from->inner(), .to = to->inner()};

    return RankJoin{.from = from, .to = to, .form = RankForm::OPTIONAL_SAME, .parts = {inner}};
  }
  auto wrapped = RankGoal{.from = from, .to = to->inner()};

  return RankJoin{.from = from, .to = to, .form = RankForm::OPTIONAL_WRAP, .parts = {wrapped}};
}

RankJoin OverloadResolver::membersJoin(const Type *from, const Type *to) const {
  auto parts = std::vector<RankGoal>();
  for (const Type *member : from->members()) {
    parts.push_back(RankGoal{.from = member, .to = to});
  }

  return RankJoin{.from = from, .to = to, .form = RankForm::WORST_MEMBER, .parts = parts};
}

RankJoin OverloadResolver::targetsJoin(const Type *from, const Type *to) const {
  auto parts = std::vector<RankGoal>();
  for (const Type *member : to->members()) {
    parts.push_back(RankGoal{.from = from, .to = member});
  }

  return RankJoin{.from = from, .to = to, .form = RankForm::BEST_TARGET, .parts = parts};
}

RankJoin OverloadResolver::resultJoin(const Type *from, const Type *to) const {
  auto value = RankGoal{.from = from->okType(), .to = to->okType()};
  auto error = RankGoal{.from = from->errType(), .to = to->errType()};

  return RankJoin{.from = from, .to = to, .form = RankForm::RESULT, .parts = {value, error}};
}

RankJoin OverloadResolver::fieldsJoin(const Type *from, const Type *to) const {
  auto parts = std::vector<RankGoal>();
  for (const Field &field : from->fields()) {
    parts.push_back(RankGoal{.from = field.type, .to = to->field(field.name)->type});
  }

  return RankJoin{.from = from, .to = to, .form = RankForm::FIELDS, .parts = parts};
}

void OverloadResolver::joinRanks(const RankJoin &join, RankRun &run) const {
  run.path.pop_back();
  auto ranks = ChildrenFirst::takeAnswers(run.answers, join.parts.size());
  int rank = this->joinedRank(join, ranks);
  run.answers.push_back(rank);
}

int OverloadResolver::joinedRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const {
  bool isFailed = llvm::is_contained(ranks, kNoRank);
  switch (join.form) {
    case RankForm::SAME:
      return ranks[0];
    case RankForm::OPTIONAL_SAME:
      return ranks[0] < 0 ? kNoRank : ranks[0];
    case RankForm::OPTIONAL_WRAP:
      return ranks[0] < 0 ? kNoRank : ranks[0] + 1;
    case RankForm::WORST_MEMBER:
      return isFailed ? kNoRank : this->worstRank(ranks);
    case RankForm::BEST_TARGET:
      return this->bestRank(ranks);
    case RankForm::RESULT:
      return this->resultRank(join, ranks);
    case RankForm::FIELDS:
      return isFailed ? kNoRank : kFieldsRank;
  }

  return kNoRank;
}

int OverloadResolver::worstRank(llvm::ArrayRef<int> ranks) const {
  int rank = 0;
  for (int inner : ranks) {
    rank = std::max(rank, inner);
  }

  return rank;
}

int OverloadResolver::bestRank(llvm::ArrayRef<int> ranks) const {
  int best = kNoRank;
  for (int inner : ranks) {
    bool isBetter = inner >= 0 && (best < 0 || inner < best);
    if (isBetter) {
      best = inner;
    }
  }
  if (best < 0) {
    return kNoRank;
  }

  return best + 1;
}

int OverloadResolver::resultRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const {
  int valueRank = this->halfRank(join.from->okType(), join.to->okType(), ranks[0]);
  int errorRank = this->halfRank(join.from->errType(), join.to->errType(), ranks[1]);
  if (valueRank < 0 || errorRank < 0) {
    return kNoRank;
  }

  return std::max(valueRank, errorRank);
}

int OverloadResolver::halfRank(const Type *from, const Type *to, int rank) const {
  if (from == to) {
    return kExactRank;
  }
  if (from->isNever()) {
    return kDirectRank;
  }
  if (from->isVoid() || to->isVoid()) {
    return kNoRank;
  }

  return rank;
}

const Type *OverloadResolver::parameterAt(const FuncSymbol *function, size_t index) const {
  bool isRest = function->hasRest && index + 1 >= function->params.size();
  if (isRest) {
    const Type *rest = function->params.back();

    return rest->isArray() ? rest->inner() : rest;
  }
  if (index >= function->params.size()) {
    return nullptr;
  }

  return function->params[index];
}

bool OverloadResolver::hasFloatLiteral(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (isa<FloatLitExpr>(node)) {
      return true;
    }
  }

  return false;
}

OverloadResolver::ResultVariantChosen
OverloadResolver::chooseVariant(llvm::ArrayRef<FunctionDecl *> variants, std::vector<Expr *> &args,
                                const std::vector<bool> &spread) {
  return this->chooseAmong(variants, args, spread, GenericVariants::ALLOWED);
}

OverloadResolver::ResultVariantChosen
OverloadResolver::choosePlainVariant(llvm::ArrayRef<FunctionDecl *> variants,
                                     std::vector<Expr *> &args, const std::vector<bool> &spread) {
  return this->chooseAmong(variants, args, spread, GenericVariants::REJECTED);
}

OverloadResolver::ResultVariantChosen
OverloadResolver::chooseAmong(llvm::ArrayRef<FunctionDecl *> variants, std::vector<Expr *> &args,
                              const std::vector<bool> &spread, GenericVariants generics) {
  if (variants.size() == 1) {
    return ok(this->uncheckedChoice(variants[0]));
  }
  auto what = this->variantsName(variants[0]);
  auto plain = this->checkNoSpread(what, args, spread);
  if (plain.isErr()) {
    return err(plain.error());
  }
  auto fit = this->fittingVariants(what, variants, args);
  if (fit.isErr()) {
    return err(fit.error());
  }
  if (fit.value().size() == 1) {
    return ok(this->uncheckedChoice(fit.value()[0]));
  }
  auto typed = this->checkArgumentsFor(fit.value(), args);
  if (typed.isErr()) {
    return err(typed.error());
  }
  auto ranked = this->rankedCandidates(what, fit.value(), args);
  if (ranked.isErr()) {
    return err(ranked.error());
  }
  auto best = this->soleBest(what, ranked.value());
  if (best.isErr()) {
    return err(best.error());
  }
  auto allowed = this->checkGenericAllowed(best.value(), generics);
  if (allowed.isErr()) {
    return err(allowed.error());
  }

  return ok(ChosenVariant{
      .function = best.value().functionDecl->sym,
      .isChecked = true,
      .typeArgs = best.value().typeArgs,
  });
}

OverloadResolver::ChosenVariant OverloadResolver::uncheckedChoice(FunctionDecl *variant) const {
  return ChosenVariant{
      .function = variant->sym,
      .isChecked = false,
      .typeArgs = {},
  };
}

std::string OverloadResolver::variantsName(const FunctionDecl *first) const {
  if (first->isConstructor()) {
    return ErrorTextVariant::ctorOf(first->cls()->name);
  }

  return ErrorTextVariant::quoted(first->name);
}

void OverloadResolver::walkLoosely(std::vector<Expr *> &args) {
  for (Expr *&arg : args) {
    if (arg->type == nullptr) {
      arg = this->exprChecker->checkExpr(arg);
    }
  }
}

OverloadResolver::ResultChoiceAllowed
OverloadResolver::checkNoSpread(llvm::StringRef what, std::vector<Expr *> &args,
                                const std::vector<bool> &spread) {
  if (llvm::is_contained(spread, true)) {
    this->walkLoosely(args);
    auto message = ErrorTextVariant::spread(what);

    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok();
}

OverloadResolver::ResultVariantsFound
OverloadResolver::fittingVariants(llvm::StringRef what, llvm::ArrayRef<FunctionDecl *> variants,
                                  std::vector<Expr *> &args) {
  size_t count = args.size();
  auto fit = std::vector<FunctionDecl *>();
  for (FunctionDecl *variant : variants) {
    if (this->isTakingCount(variant->sym, count)) {
      fit.push_back(variant);
    }
  }
  if (fit.empty()) {
    auto list = ErrorTextVariant::list(variants);
    this->walkLoosely(args);
    auto message = ErrorTextVariant::count(what, count, list);

    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::NO_VARIANT_FOR_COUNT,
        .message = message,
    });
  }

  return ok(fit);
}

bool OverloadResolver::isTakingCount(const FuncSymbol *function, size_t count) const {
  bool isEnough = count >= function->minArgs;
  bool isNotTooMany = function->hasRest || count <= function->params.size();

  return isEnough && isNotTooMany;
}

OverloadResolver::ResultChoiceAllowed
OverloadResolver::checkArgumentsFor(llvm::ArrayRef<FunctionDecl *> fit, std::vector<Expr *> &args) {
  for (size_t index = 0; index < args.size(); ++index) {
    const Type *expected = this->expectedArgument(fit, args[index], index);
    bool isUsable = expected != nullptr && !expected->isError();
    args[index] = this->exprChecker->checkExpr(args[index], isUsable ? expected : nullptr);
  }
  for (const Expr *arg : args) {
    if (arg->type->isError()) {
      return err(OverloadResolverChooseVariantError{
          .code = OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
  }

  return ok();
}

const Type *OverloadResolver::expectedArgument(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                               size_t index) const {
  const Type *common = this->commonParameter(fit, index);
  if (common != nullptr) {
    return common;
  }
  const Type *shape = this->soleObjectShape(fit, arg, index);
  if (shape != nullptr) {
    return shape;
  }

  return this->decimalForLiteral(fit, arg, index);
}

const Type *OverloadResolver::commonParameter(llvm::ArrayRef<FunctionDecl *> fit,
                                              size_t index) const {
  const Type *common = this->parameterAt(fit[0]->sym, index);
  for (FunctionDecl *variant : fit) {
    bool isSame = this->parameterAt(variant->sym, index) == common;
    bool isOpen =
        common != nullptr && GenericCheckHelpers::mentionsAnyOf(common, variant->sym->typeParams);
    if (!isSame || isOpen) {
      return nullptr;
    }
  }

  return common;
}

const Type *OverloadResolver::soleObjectShape(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                              size_t index) const {
  if (!isa<ObjectLitExpr>(arg)) {
    return nullptr;
  }
  const Type *shape = nullptr;
  int changes = 0;
  for (FunctionDecl *variant : fit) {
    const Type *param = this->parameterAt(variant->sym, index);
    if (this->isPlainObjectParam(param) && param != shape) {
      shape = param;
      changes += 1;
    }
  }
  if (changes != 1) {
    return nullptr;
  }

  return shape;
}

bool OverloadResolver::isPlainObjectParam(const Type *param) const {
  if (param == nullptr) {
    return false;
  }
  const Type *bare = param->nonOptional();

  return bare->isObject() && !bare->isClass() && !bare->isAnyObject() && !bare->isTuple();
}

const Type *OverloadResolver::decimalForLiteral(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                                size_t index) const {
  if (!this->decimalChecker->isLiteralTree(arg) || !this->hasFloatLiteral(arg)) {
    return nullptr;
  }
  bool hasDecimal = false;
  bool hasDouble = false;
  for (FunctionDecl *variant : fit) {
    const Type *param = this->parameterAt(variant->sym, index)->nonOptional();
    hasDecimal = hasDecimal || param->isDecimal();
    hasDouble = hasDouble || param->isFloat();
  }
  if (hasDecimal && !hasDouble) {
    return Type::decimalTy();
  }

  return nullptr;
}

OverloadResolver::ResultCandidatesFit
OverloadResolver::rankedCandidates(llvm::StringRef what, llvm::ArrayRef<FunctionDecl *> fit,
                                   std::vector<Expr *> &args) {
  auto ranking = VariantRanking();
  for (FunctionDecl *variant : fit) {
    this->rankVariant(variant, args, ranking);
  }
  if (ranking.applicable.empty()) {
    auto message = ErrorTextVariant::noFit(what, ranking.why, ranking.numberHint);

    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::NO_VARIANT_FITS,
        .message = message,
    });
  }

  return ok(ranking.applicable);
}

void OverloadResolver::rankVariant(FunctionDecl *variant, std::vector<Expr *> &args,
                                   VariantRanking &ranking) {
  FuncSymbol *function = variant->sym;
  auto candidate = Candidate{
      .functionDecl = variant,
      .ranks = {},
      .isLeavingOut = function->hasRest || args.size() < function->params.size(),
      .typeArgs = {},
      .isAdapted = false,
  };
  bool isInferred = !function->isGeneric() ||
                    this->typeArgInferrer->inferFromArguments(function, args, candidate.typeArgs);
  if (!isInferred) {
    ranking.why += ErrorTextVariant::typeParams(ErrorTextVariant::text(variant));
    return;
  }
  for (size_t index = 0; index < args.size(); ++index) {
    const Type *param = this->candidateParam(candidate, index);
    int rank = this->argumentRank(args[index], param, candidate);
    if (rank < 0) {
      this->noteMismatch(variant, args[index], param, index, ranking);
      return;
    }
    candidate.ranks.push_back(rank);
  }
  ranking.applicable.push_back(std::move(candidate));
}

const Type *OverloadResolver::candidateParam(const Candidate &candidate, size_t index) const {
  const FuncSymbol *function = candidate.functionDecl->sym;
  const Type *param = this->parameterAt(function, index);
  if (!function->isGeneric()) {
    return param;
  }

  return this->typeParams->substituteValue(param, function->typeParams, candidate.typeArgs);
}

int OverloadResolver::argumentRank(const Expr *arg, const Type *param, Candidate &candidate) const {
  int rank = this->conversionRank(arg->type, param);
  if (rank >= 0 || !this->typeRelations->isLiteralAdapting(arg, param)) {
    return rank;
  }
  candidate.isAdapted = true;

  return kDirectRank;
}

void OverloadResolver::noteMismatch(const FunctionDecl *variant, const Expr *arg, const Type *param,
                                    size_t index, VariantRanking &ranking) const {
  auto shown = ErrorTextVariant::text(variant);
  auto mismatch = ErrorTextVariant::mismatch(shown, param->name(), index + 1, arg->type->name());
  if (ranking.numberHint.empty()) {
    ranking.numberHint = this->typeRelations->conversionHint(arg, param);
  }
  ranking.why += "; " + mismatch;
}

OverloadResolver::ResultBestCandidate
OverloadResolver::soleBest(llvm::StringRef what, llvm::ArrayRef<Candidate> applicable) const {
  for (const Candidate &candidate : applicable) {
    if (this->isBetterThanOthers(candidate, applicable)) {
      return ok(candidate);
    }
  }
  auto tied = std::vector<FunctionDecl *>();
  bool hasOnlyAdapted = true;
  for (const Candidate &candidate : applicable) {
    tied.push_back(candidate.functionDecl);
    hasOnlyAdapted = hasOnlyAdapted && candidate.isAdapted;
  }
  auto list = ErrorTextVariant::list(tied);
  auto message = ErrorTextVariant::tie(what, list, hasOnlyAdapted);

  return err(OverloadResolverChooseVariantError{
      .code = OverloadResolverChooseVariantErrorCode::SEVERAL_VARIANTS_FIT,
      .message = message,
  });
}

bool OverloadResolver::isBetterThanOthers(const Candidate &candidate,
                                          llvm::ArrayRef<Candidate> applicable) const {
  for (const Candidate &other : applicable) {
    if (&other != &candidate && !this->isBetter(candidate, other)) {
      return false;
    }
  }

  return true;
}

bool OverloadResolver::isBetter(const Candidate &candidate, const Candidate &other) const {
  bool isStrictly = false;
  for (size_t index = 0; index < candidate.ranks.size(); ++index) {
    if (candidate.ranks[index] > other.ranks[index]) {
      return false;
    }
    isStrictly = isStrictly || candidate.ranks[index] < other.ranks[index];
  }
  bool isFullerCall = !candidate.isLeavingOut && other.isLeavingOut;
  bool isPlainOverGeneric =
      !candidate.functionDecl->sym->isGeneric() && other.functionDecl->sym->isGeneric();

  return isStrictly || isFullerCall || isPlainOverGeneric;
}

OverloadResolver::ResultChoiceAllowed
OverloadResolver::checkGenericAllowed(const Candidate &candidate, GenericVariants generics) const {
  bool isRejected = generics == GenericVariants::REJECTED;
  if (isRejected && candidate.functionDecl->sym->isGeneric()) {
    auto shown = ErrorTextVariant::text(candidate.functionDecl);
    auto message = ErrorTextVariant::generic(shown);

    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::GENERIC_VARIANT,
        .message = message,
    });
  }

  return ok();
}

Expr *OverloadResolver::callGenericMethod(CallExpr *call, FuncSymbol *function,
                                          llvm::ArrayRef<const Type *> bound, llvm::SMLoc loc) {
  return this->callGeneric(call, function, bound, loc, GenericCallee::METHOD);
}

Expr *OverloadResolver::callGenericFunction(CallExpr *call, FuncSymbol *function,
                                            llvm::ArrayRef<const Type *> bound, llvm::SMLoc loc) {
  return this->callGeneric(call, function, bound, loc, GenericCallee::FUNCTION);
}

Expr *OverloadResolver::callGeneric(CallExpr *call, FuncSymbol *function,
                                    llvm::ArrayRef<const Type *> bound, llvm::SMLoc loc,
                                    GenericCallee kind) {
  this->state.getGenericStore().enterInstanceLoc(loc);
  auto params = this->boundParams(function, bound);
  call->type = this->boundReturnType(function, bound);
  this->state.getGenericStore().leaveInstanceLoc();
  FuncSymbol *callee = this->emittedCallee(function, bound, loc);
  this->setGenericTarget(call, callee, kind);
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member != nullptr && kind == GenericCallee::METHOD) {
    member->type = this->returnInferrer->functionType(callee);
  }
  auto given = std::vector<bool>(call->args.size(), true);

  return this->argumentChecker->checkArguments(call, params, function->minArgs, function->hasRest,
                                               loc, &given);
}

std::vector<const Type *> OverloadResolver::boundParams(const FuncSymbol *function,
                                                        llvm::ArrayRef<const Type *> bound) {
  auto params = std::vector<const Type *>();
  for (const Type *param : function->params) {
    const auto *type = this->typeParams->substituteValue(param, function->typeParams, bound);
    params.push_back(type);
  }

  return params;
}

const Type *OverloadResolver::boundReturnType(FuncSymbol *function,
                                              llvm::ArrayRef<const Type *> bound) {
  const auto *returnType = function->getReturnType();
  if (returnType == nullptr) {
    return Type::error();
  }

  return this->typeParams->substitute(returnType, function->typeParams, bound);
}

FuncSymbol *OverloadResolver::emittedCallee(FuncSymbol *function,
                                            llvm::ArrayRef<const Type *> bound, llvm::SMLoc loc) {
  if (!this->isInstantiatedNow(function)) {
    return function;
  }
  auto *instance = this->genericInstantiator->instantiate(function, bound, loc);
  if (instance == nullptr) {
    return function;
  }

  return instance;
}

bool OverloadResolver::isInstantiatedNow(const FuncSymbol *function) const {
  const ClassDecl *classDecl = function->decl->cls();
  bool isOwnerEmitted =
      classDecl == nullptr || function->decl->isStatic() || classDecl->isEmitted();
  bool isSettled = !this->state.getFunctionStack().isInsideTrial() && !function->inferring &&
                   function->hasReturnType();

  return isSettled && isOwnerEmitted;
}

void OverloadResolver::setGenericTarget(CallExpr *call, FuncSymbol *callee,
                                        GenericCallee kind) const {
  if (call->superCall()) {
    call->target = SuperCall{.method = callee};
    return;
  }
  switch (kind) {
    case GenericCallee::METHOD:
      call->target = MethodCall{.method = callee};
      break;
    case GenericCallee::FUNCTION:
      call->target = FunctionCall{.func = callee};
      break;
  }
}

OverloadResolver::ResultVariantOfType
OverloadResolver::variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                                 const MemberExpr *member) {
  if (variants.size() == 1) {
    return ok(variants[0]->sym);
  }
  auto matching = this->variantsOfType(variants, expected);
  if (matching.size() == 1) {
    return ok(matching[0]->sym);
  }
  auto type = this->signatureText(variants.back());
  auto object = this->shownObject(member);
  auto list = ErrorTextVariant::list(variants);
  auto message = ErrorTextVariantType::several(member->name, list, type, object);

  return err(OverloadResolverVariantForTypeError{
      .code = OverloadResolverVariantForTypeErrorCode::SEVERAL_VARIANTS,
      .message = message,
  });
}

std::vector<FunctionDecl *>
OverloadResolver::variantsOfType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected) {
  const auto *callback = this->state.getFunctionStack().getExpectedCallback();
  bool isFunctionWanted = expected != nullptr && expected->nonOptional()->isFunction();
  auto matching = std::vector<FunctionDecl *>();
  for (FunctionDecl *variant : variants) {
    bool isMatching = isFunctionWanted ? this->isOfType(variant, expected->nonOptional())
                                       : this->isFittingCallback(variant->sym, callback);
    if (isMatching) {
      matching.push_back(variant);
    }
  }

  return matching;
}

bool OverloadResolver::isOfType(const FunctionDecl *variant, const Type *wanted) {
  return this->returnInferrer->functionType(variant->sym) == wanted;
}

bool OverloadResolver::isFittingCallback(const FuncSymbol *function,
                                         const Signature *callback) const {
  if (callback == nullptr) {
    return false;
  }
  size_t given = callback->params.size();
  bool isCountFitting =
      !function->hasRest && function->minArgs <= given && function->params.size() <= given;
  if (!isCountFitting) {
    return false;
  }
  for (size_t index = 0; index < function->params.size(); ++index) {
    const Type *param = function->params[index];
    const Type *passed = callback->params[index];
    bool isSame = param == passed || (param->isOptional() && param->inner() == passed);
    if (!isSame) {
      return false;
    }
  }

  return true;
}

std::string OverloadResolver::signatureText(const FunctionDecl *variant) const {
  FuncSymbol *function = variant->sym;
  size_t count = std::min(variant->params.size(), function->params.size());
  std::string text = "(";
  for (size_t index = 0; index < count; ++index) {
    if (index > 0) {
      text += ", ";
    }
    text += variant->params[index].label().str() + ": " + function->params[index]->name().str();
  }

  return text + ") => " + function->getReturnType()->name().str();
}

std::string OverloadResolver::shownObject(const MemberExpr *member) const {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (name == nullptr) {
    return "o";
  }

  return name->name.str();
}

std::vector<FunctionDecl *> OverloadResolver::signatureVariants(const Type *interfaceType,
                                                                const Overload &overload) {
  auto &classes = this->state.getClassStore();
  auto known = classes.getSignatureVariants(interfaceType, overload.name);
  if (!known.empty()) {
    return known;
  }
  auto variants = std::vector<FunctionDecl *>();
  for (const Type *signature : overload.signatures) {
    variants.push_back(this->signatureVariant(overload, signature));
  }
  classes.rememberSignatureVariants(interfaceType, overload.name, variants);

  return variants;
}

FunctionDecl *OverloadResolver::signatureVariant(const Overload &overload, const Type *signature) {
  auto name = this->state.getAst().keepName(overload.name);
  auto *functionDecl = this->state.getAst().make<FunctionDecl>(llvm::SMLoc(), name);
  auto *function = this->state.getAst().newFunc();
  function->setCompleter(this->state.getCompleter());
  function->name = functionDecl->name;
  function->decl = functionDecl;
  function->params.assign(signature->params().begin(), signature->params().end());
  function->minArgs = signature->minArgs();
  function->hasRest = signature->hasRest();
  function->setReturnType(signature->ret());
  for (size_t index = 0; index < signature->params().size(); ++index) {
    functionDecl->params.push_back(this->signatureParam(signature, index));
  }
  functionDecl->sym = function;

  return functionDecl;
}

Param OverloadResolver::signatureParam(const Type *signature, size_t index) {
  auto letter = std::string(1, char('a' + index % 26));
  auto param = Param();
  param.name = this->state.getAst().keepName(letter);
  param.declaredType = signature->params()[index];
  bool isRest = signature->hasRest() && index + 1 == signature->params().size();
  if (isRest) {
    param.form = RestParam{};
  } else if (index >= signature->minArgs()) {
    param.form = OptionalParam{};
  }

  return param;
}

Expr *OverloadResolver::checkOverloadCall(CallExpr *call, MemberExpr *member) {
  const Type *interfaceType = member->object->type;
  const Overload &overload = *interfaceType->overload(member->name);
  call->type = member->type = Type::error();
  auto variants = this->signatureVariants(interfaceType, overload);
  auto chosen = this->choosePlainVariant(variants, call->args, call->spread);
  if (chosen.isErr() &&
      chosen.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), chosen.error().message);
  }
  if (chosen.isErr()) {
    return call;
  }
  FuncSymbol *function = chosen.value().function;
  int signature = this->signatureIndex(variants, function);
  call->target = OverloadCall{.signature = signature};
  member->type = overload.signatures[size_t(signature)];
  call->type = function->getKnownReturnType();
  this->flowChecker->calledFunction();
  size_t checkedCount = chosen.value().isChecked ? call->args.size() : 0;
  auto given = std::vector<bool>(checkedCount, true);

  return this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                               function->hasRest, member->loc(), &given);
}

int OverloadResolver::signatureIndex(llvm::ArrayRef<FunctionDecl *> variants,
                                     const FuncSymbol *function) const {
  auto found = llvm::find(variants, function->decl);

  return int(found - variants.begin());
}

Expr *OverloadResolver::checkOverloadMember(MemberExpr *member, const Type *expected) {
  const Type *interfaceType = member->object->type;
  const Overload &overload = *interfaceType->overload(member->name);
  member->type = Type::error();
  auto variants = this->signatureVariants(interfaceType, overload);
  auto variant = this->variantForType(variants, expected, member);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(member->loc(), variant.error().message);

    return member;
  }
  int signature = this->signatureIndex(variants, variant.value());
  member->form = OverloadMember{.signature = signature};
  member->type = overload.signatures[size_t(signature)];

  return member;
}

}
