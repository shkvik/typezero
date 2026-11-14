#include "compiler/semantic/code/classes/overload_choice/candidate_ranking/candidate_ranking.h"
#include "compiler/program/library/regex/match_member_table/match_member_table.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr int kNoRank = -1;
constexpr int kExactRank = 0;
constexpr int kDirectRank = 1;
constexpr int kFieldsRank = 2;
constexpr int kJsonRank = 3;

}

CandidateRanking::CandidateRanking(SemanticState &owner, ChoiceRules &sharedChoiceRules)
    : state(owner),
      choiceRules(sharedChoiceRules) {}

void CandidateRanking::rankVariants(const RankVariants &task) {
  const ChoiceDraft &draft = task.draft;
  auto typed = this->checkArgumentsTyped(*draft.args);
  if (typed.isErr()) {
    return this->state.code.overloads.pushChoice(this->choiceRules.failedChoice(typed.error()));
  }

  return this->nextRankedVariant(NextRankedVariant{.draft = draft});
}

CandidateRanking::ResultChoiceAllowed
CandidateRanking::checkArgumentsTyped(llvm::ArrayRef<Expr *> args) const {
  for (const Expr *arg : args) {
    if (arg->type->isError()) {
      return err(CodeClassesRegistryChooseVariantError{
          .code = CodeClassesRegistryChooseVariantErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
  }

  return ok();
}

void CandidateRanking::nextRankedVariant(const NextRankedVariant &task) {
  auto draft = task.draft;
  std::vector<Expr *> &args = *draft.args;
  while (draft.index < draft.fit.size()) {
    FunctionDecl *variant = draft.fit[draft.index];
    FuncSymbol *function = variant->sym;
    if (!function->isGeneric()) {
      auto params = this->plainParams(function, args.size());
      this->rankVariant(variant, args, params, {}, draft.ranking);
      draft.index += 1;
      continue;
    }
    auto typeArgs = std::vector<const Type *>();
    bool isInferred = this->state.generics.inference.inferFromArguments(function, args, typeArgs);
    if (isInferred) {
      return this->pushGenericParams(std::move(draft), std::move(typeArgs));
    }
    auto shown = ErrorTextVariant::text(variant);
    draft.ranking.why += ErrorTextVariant::typeParams(shown);
    draft.index += 1;
  }

  return this->finishChoice(draft);
}

std::vector<const Type *> CandidateRanking::plainParams(const FuncSymbol *function,
                                                        size_t count) const {
  auto params = std::vector<const Type *>();
  for (size_t index = 0; index < count; ++index) {
    params.push_back(this->choiceRules.parameterAt(function, index));
  }

  return params;
}

void CandidateRanking::pushGenericParams(ChoiceDraft draft, std::vector<const Type *> typeArgs) {
  auto &tasks = this->state.walk.tasks;
  const FuncSymbol *function = draft.fit[draft.index]->sym;
  size_t count = draft.args->size();
  auto params = this->plainParams(function, count);
  auto typeParams = function->typeParams;
  tasks.pushTask(AfterGenericParams{.draft = std::move(draft), .typeArgs = typeArgs});
  for (const Type *param : llvm::reverse(params)) {
    tasks.pushTask(SubstituteValueType{.type = param, .params = typeParams, .args = typeArgs});
  }
}

void CandidateRanking::afterGenericParams(const AfterGenericParams &task) {
  auto draft = task.draft;
  FunctionDecl *variant = draft.fit[draft.index];
  std::vector<Expr *> &args = *draft.args;
  auto params = std::vector<const Type *>(args.size());
  for (size_t index = params.size(); index > 0; --index) {
    params[index - 1] = this->state.walk.tasks.takeAnswer();
  }
  this->rankVariant(variant, args, params, task.typeArgs, draft.ranking);
  draft.index += 1;

  return this->nextRankedVariant(NextRankedVariant{.draft = std::move(draft)});
}

void CandidateRanking::rankVariant(FunctionDecl *variant, llvm::ArrayRef<Expr *> args,
                                   llvm::ArrayRef<const Type *> params,
                                   const std::vector<const Type *> &typeArgs,
                                   VariantRanking &ranking) const {
  FuncSymbol *function = variant->sym;
  auto candidate = VariantCandidate{
      .functionDecl = variant,
      .ranks = {},
      .isLeavingOut = function->hasRest || args.size() < function->params.size(),
      .typeArgs = typeArgs,
      .isAdapted = false,
  };
  for (size_t index = 0; index < args.size(); ++index) {
    int rank = this->argumentRank(args[index], params[index], candidate);
    if (rank < 0) {
      return this->noteMismatch(variant, args[index], params[index], index, ranking);
    }
    candidate.ranks.push_back(rank);
  }
  ranking.applicable.push_back(std::move(candidate));
}

void CandidateRanking::finishChoice(const ChoiceDraft &draft) {
  auto &overloads = this->state.code.overloads;
  auto ranked = this->checkRankedCandidates(draft.what, draft.ranking);
  if (ranked.isErr()) {
    return overloads.pushChoice(this->choiceRules.failedChoice(ranked.error()));
  }
  auto best = this->checkSoleBest(draft.what, draft.ranking.applicable);
  if (best.isErr()) {
    return overloads.pushChoice(this->choiceRules.failedChoice(best.error()));
  }
  auto allowed = this->checkGenericAllowed(best.value(), draft.generics);
  if (allowed.isErr()) {
    return overloads.pushChoice(this->choiceRules.failedChoice(allowed.error()));
  }

  return overloads.pushChoice(VariantChoice{
      .function = best.value().functionDecl->sym,
      .isChecked = true,
      .typeArgs = best.value().typeArgs,
      .isReported = false,
      .message = "",
  });
}

CandidateRanking::ResultChoiceAllowed
CandidateRanking::checkRankedCandidates(llvm::StringRef what, const VariantRanking &ranking) const {
  if (ranking.applicable.empty()) {
    auto message = ErrorTextVariant::noFit(what, ranking.why, ranking.numberHint);

    return err(CodeClassesRegistryChooseVariantError{
        .code = CodeClassesRegistryChooseVariantErrorCode::NO_VARIANT_FITS,
        .message = message,
    });
  }

  return ok();
}

int CandidateRanking::conversionRank(const Type *from, const Type *to) const {
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

void CandidateRanking::enterRank(const RankGoal &goal, RankRun &run) const {
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

std::optional<int> CandidateRanking::rankLeaf(const Type *from, const Type *to) const {
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

int CandidateRanking::jsonRank(const Type *from) const {
  bool isJsonArray = from->isArray() && from->inner()->isJson();
  bool isJsonValue = from->isString() || from->isNumeric() || from->isBool() || isJsonArray;
  if (!isJsonValue) {
    return kNoRank;
  }

  return kJsonRank;
}

int CandidateRanking::iteratorRank(const Type *from, const Type *to) const {
  bool isSameReturn = from->returned() == to->returned() || to->returned()->isVoid();
  bool isSameSent = from->sent() == to->sent() || to->sent()->isVoid();
  bool isSame = from->yielded() == to->yielded() && isSameReturn && isSameSent;
  if (!isSame) {
    return kNoRank;
  }

  return kDirectRank;
}

int CandidateRanking::bytesRank(const Type *from, const Type *to) const {
  bool isBufferView = from->isBuffer() && to->bytes() == Type::Bytes::Uint8;
  if (!isBufferView) {
    return kNoRank;
  }

  return kDirectRank;
}

int CandidateRanking::matchRank(const Type *from, const Type *to) const {
  bool isPlainMatch = from->isMatch() && to->isMatch() && to->matchGroups() < 0 &&
                      from->field(MatchMemberTable::getName(MatchMember::INDICES)) == nullptr;
  if (!isPlainMatch) {
    return kNoRank;
  }

  return kDirectRank;
}

bool CandidateRanking::isOpaqueRanked(const Type *type) const {
  return type->isMatch() || type->isRegExp() || type->isDate() || type->isIntl() ||
         type->isCrypto() || type->isWeakRef();
}

std::optional<int> CandidateRanking::objectRank(const Type *from, const Type *to) const {
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

int CandidateRanking::subtypeDistance(const Type *from, const Type *to) const {
  if (!from->isObject() || !to->isObject()) {
    return 0;
  }

  return TypeQueries::subtypeDistance(from, to);
}

bool CandidateRanking::hasSameFields(const Type *from, const Type *to) const {
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

bool CandidateRanking::isShapeObject(const Type *type) const {
  return type->isObject() && !type->isClass() && !type->isAnyObject() && !type->isTuple() &&
         type->overloads().empty();
}

bool CandidateRanking::isSameShape(const Type *from, const Type *to) const {
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

RankJoin CandidateRanking::rankJoin(const Type *from, const Type *to) const {
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

RankJoin CandidateRanking::optionalJoin(const Type *from, const Type *to) const {
  if (from->isOptional()) {
    auto inner = RankGoal{.from = from->inner(), .to = to->inner()};

    return RankJoin{.from = from, .to = to, .form = RankForm::OPTIONAL_SAME, .parts = {inner}};
  }
  auto wrapped = RankGoal{.from = from, .to = to->inner()};

  return RankJoin{.from = from, .to = to, .form = RankForm::OPTIONAL_WRAP, .parts = {wrapped}};
}

RankJoin CandidateRanking::membersJoin(const Type *from, const Type *to) const {
  auto parts = std::vector<RankGoal>();
  for (const Type *member : from->members()) {
    parts.push_back(RankGoal{.from = member, .to = to});
  }

  return RankJoin{.from = from, .to = to, .form = RankForm::WORST_MEMBER, .parts = parts};
}

RankJoin CandidateRanking::targetsJoin(const Type *from, const Type *to) const {
  auto parts = std::vector<RankGoal>();
  for (const Type *member : to->members()) {
    parts.push_back(RankGoal{.from = from, .to = member});
  }

  return RankJoin{.from = from, .to = to, .form = RankForm::BEST_TARGET, .parts = parts};
}

RankJoin CandidateRanking::resultJoin(const Type *from, const Type *to) const {
  auto value = RankGoal{.from = from->okType(), .to = to->okType()};
  auto error = RankGoal{.from = from->errType(), .to = to->errType()};

  return RankJoin{.from = from, .to = to, .form = RankForm::RESULT, .parts = {value, error}};
}

RankJoin CandidateRanking::fieldsJoin(const Type *from, const Type *to) const {
  auto parts = std::vector<RankGoal>();
  for (const Field &field : from->fields()) {
    parts.push_back(RankGoal{.from = field.type, .to = to->field(field.name)->type});
  }

  return RankJoin{.from = from, .to = to, .form = RankForm::FIELDS, .parts = parts};
}

void CandidateRanking::joinRanks(const RankJoin &join, RankRun &run) const {
  run.path.pop_back();
  auto ranks = ChildrenFirst::takeAnswers(run.answers, join.parts.size());
  int rank = this->joinedRank(join, ranks);
  run.answers.push_back(rank);
}

int CandidateRanking::joinedRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const {
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

int CandidateRanking::worstRank(llvm::ArrayRef<int> ranks) const {
  int rank = 0;
  for (int inner : ranks) {
    rank = std::max(rank, inner);
  }

  return rank;
}

int CandidateRanking::bestRank(llvm::ArrayRef<int> ranks) const {
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

int CandidateRanking::resultRank(const RankJoin &join, llvm::ArrayRef<int> ranks) const {
  int valueRank = this->halfRank(join.from->okType(), join.to->okType(), ranks[0]);
  int errorRank = this->halfRank(join.from->errType(), join.to->errType(), ranks[1]);
  if (valueRank < 0 || errorRank < 0) {
    return kNoRank;
  }

  return std::max(valueRank, errorRank);
}

int CandidateRanking::halfRank(const Type *from, const Type *to, int rank) const {
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

int CandidateRanking::argumentRank(const Expr *arg, const Type *param,
                                   VariantCandidate &candidate) const {
  int rank = this->conversionRank(arg->type, param);
  if (rank >= 0 || !this->state.code.conversions.isLiteralAdapting(arg, param)) {
    return rank;
  }
  candidate.isAdapted = true;

  return kDirectRank;
}

void CandidateRanking::noteMismatch(const FunctionDecl *variant, const Expr *arg, const Type *param,
                                    size_t index, VariantRanking &ranking) const {
  auto shown = ErrorTextVariant::text(variant);
  auto mismatch = ErrorTextVariant::mismatch(shown, param->name(), index + 1, arg->type->name());
  if (ranking.numberHint.empty()) {
    ranking.numberHint = this->state.code.conversions.conversionHint(arg, param);
  }
  ranking.why += "; " + mismatch;
}

CandidateRanking::ResultBestCandidate
CandidateRanking::checkSoleBest(llvm::StringRef what,
                                llvm::ArrayRef<VariantCandidate> applicable) const {
  for (const VariantCandidate &candidate : applicable) {
    if (this->isBetterThanOthers(candidate, applicable)) {
      return ok(candidate);
    }
  }
  auto tied = std::vector<FunctionDecl *>();
  bool hasOnlyAdapted = true;
  for (const VariantCandidate &candidate : applicable) {
    tied.push_back(candidate.functionDecl);
    hasOnlyAdapted = hasOnlyAdapted && candidate.isAdapted;
  }
  auto list = ErrorTextVariant::list(tied);
  auto message = ErrorTextVariant::tie(what, list, hasOnlyAdapted);

  return err(CodeClassesRegistryChooseVariantError{
      .code = CodeClassesRegistryChooseVariantErrorCode::SEVERAL_VARIANTS_FIT,
      .message = message,
  });
}

bool CandidateRanking::isBetterThanOthers(const VariantCandidate &candidate,
                                          llvm::ArrayRef<VariantCandidate> applicable) const {
  for (const VariantCandidate &other : applicable) {
    if (&other != &candidate && !this->isBetter(candidate, other)) {
      return false;
    }
  }

  return true;
}

bool CandidateRanking::isBetter(const VariantCandidate &candidate,
                                const VariantCandidate &other) const {
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

CandidateRanking::ResultChoiceAllowed
CandidateRanking::checkGenericAllowed(const VariantCandidate &candidate,
                                      GenericVariants generics) const {
  bool isRejected = generics == GenericVariants::REJECTED;
  if (isRejected && candidate.functionDecl->sym->isGeneric()) {
    auto shown = ErrorTextVariant::text(candidate.functionDecl);
    auto message = ErrorTextVariant::generic(shown);

    return err(CodeClassesRegistryChooseVariantError{
        .code = CodeClassesRegistryChooseVariantErrorCode::GENERIC_VARIANT,
        .message = message,
    });
  }

  return ok();
}

}
