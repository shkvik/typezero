#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/extends_relation/extends_relation.h"
#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

namespace {

constexpr unsigned kMaxFitsDepth = 8;
constexpr size_t kFitsWayCount = 3;
constexpr const char *kUnnamedConditional = "conditional type";

}

ConditionalTypes::ConditionalTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeParams = &all.typeParams;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->typeOperators = &all.typeOperators;
  this->returnInferrer = &all.returnInferrer;
  this->extendsRelation = &all.extendsRelation;
  this->topLevelChecker = &all.topLevelChecker;
}

bool ConditionalTypes::isBinding(const Stmt *decl) const {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(decl);

  return alias != nullptr && alias->resolved != nullptr && alias->typeParams.empty() &&
         alias->type.kind() == TypeRefKind::NAME_REF && alias->type.name.empty();
}

llvm::ArrayRef<const Type *> ConditionalTypes::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

std::vector<llvm::StringRef> ConditionalTypes::namesIn(const TypeRef &ref) const {
  auto names = std::vector<llvm::StringRef>();
  for (const TypeRef *part : TypeRefParts::collect(ref)) {
    bool isNamed =
        part->kind() == TypeRefKind::NAME_REF || part->kind() == TypeRefKind::GENERIC_REF;
    if (isNamed && !part->name.empty() && !llvm::is_contained(names, part->name)) {
      names.push_back(part->name);
    }
  }

  return names;
}

std::vector<InferRef> ConditionalTypes::infersIn(const TypeRef &ref) const {
  auto infers = std::vector<InferRef>();
  auto stack = std::vector<SpreadRef>{SpreadRef{.ref = &ref, .isSpread = false}};
  while (!stack.empty()) {
    SpreadRef next = stack.back();
    stack.pop_back();
    TypeRefKind kind = next.ref->kind();
    if (kind == TypeRefKind::INFER_REF) {
      this->noteInfer(next, infers);
    }
    if (kind != TypeRefKind::CONDITIONAL_REF) {
      this->pushSpreadArgs(*next.ref, stack);
    }
  }

  return infers;
}

void ConditionalTypes::noteInfer(SpreadRef part, std::vector<InferRef> &infers) const {
  for (InferRef &known : infers) {
    if (known.ref->name == part.ref->name) {
      known.isList = known.isList || part.isSpread;
      return;
    }
  }
  infers.push_back(InferRef{.ref = part.ref, .isList = part.isSpread});
}

void ConditionalTypes::pushSpreadArgs(const TypeRef &ref, std::vector<SpreadRef> &stack) const {
  for (size_t index = ref.args.size(); index > 0; --index) {
    bool isSpread = this->isSpreadAt(ref, index - 1);
    stack.push_back(SpreadRef{.ref = &ref.args[index - 1], .isSpread = isSpread});
  }
}

bool ConditionalTypes::isSpreadAt(const TypeRef &ref, size_t index) const {
  bool isTupleSpread =
      ref.kind() == TypeRefKind::TUPLE_REF && index < ref.spread().size() && ref.spread()[index];
  bool isRestParam =
      ref.kind() == TypeRefKind::FUNCTION_REF && ref.rest() && index + 2 == ref.args.size();

  return isTupleSpread || isRestParam;
}

bool ConditionalTypes::isPlainParam(const Type *type) const {
  return type->kind() == Type::Kind::Param && type->typeOp() == TypeOp::None;
}

bool ConditionalTypes::isMentioningParamBesides(const Type *type,
                                                llvm::ArrayRef<const Type *> allowed) const {
  auto seen = llvm::SmallPtrSet<const Type *, 4>();
  auto stack = std::vector<const Type *>{type};
  while (!stack.empty()) {
    const Type *next = stack.back();
    stack.pop_back();
    bool isMentioning = next != nullptr && TypeQueries::mentionsParam(next);
    bool isParam = isMentioning && this->isPlainParam(next);
    if (isParam && !llvm::is_contained(allowed, next)) {
      return true;
    }
    if (isMentioning && !isParam) {
      auto parts = this->otherParamParts(next, seen);
      stack.insert(stack.end(), parts.rbegin(), parts.rend());
    }
  }

  return false;
}

std::vector<const Type *>
ConditionalTypes::otherParamParts(const Type *type,
                                  llvm::SmallPtrSetImpl<const Type *> &seen) const {
  switch (type->kind()) {
    case Type::Kind::Param:
      return this->operandParts(type);
    case Type::Kind::Optional:
    case Type::Kind::Array:
    case Type::Kind::Promise:
      return {type->inner()};
    case Type::Kind::Result:
      return {type->okType(), type->errType()};
    case Type::Kind::Function:
      return this->functionParts(type);
    case Type::Kind::Iterator:
      return {type->yielded(), type->returned(), type->sent()};
    case Type::Kind::Dict:
      return {type->keyType(), type->valueType()};
    case Type::Kind::Union:
      return std::vector<const Type *>(type->members().begin(), type->members().end());
    case Type::Kind::Object:
      return this->objectParamParts(type, seen);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Bool:
    case Type::Kind::Int:
    case Type::Kind::Double:
    case Type::Kind::Decimal:
    case Type::Kind::String:
    case Type::Kind::Never:
    case Type::Kind::Json:
    case Type::Kind::Literal:
    case Type::Kind::Unknown:
    case Type::Kind::ClassRef:
      return {};
  }

  return {};
}

std::vector<const Type *> ConditionalTypes::operandParts(const Type *type) const {
  llvm::ArrayRef<const Type *> operands = type->operands();
  if (type->typeOp() == TypeOp::Conditional) {
    return std::vector<const Type *>(operands.begin() + 1, operands.end());
  }

  return std::vector<const Type *>(operands.begin(), operands.end());
}

std::vector<const Type *> ConditionalTypes::functionParts(const Type *function) const {
  auto parts = std::vector<const Type *>{function->ret()};
  parts.insert(parts.end(), function->params().begin(), function->params().end());

  return parts;
}

std::vector<const Type *>
ConditionalTypes::objectParamParts(const Type *type,
                                   llvm::SmallPtrSetImpl<const Type *> &seen) const {
  if (type->isAsyncIterator()) {
    return {type->yielded(), type->returned(), type->sent()};
  }
  if (!type->typeArgs().empty()) {
    return std::vector<const Type *>(type->typeArgs().begin(), type->typeArgs().end());
  }
  auto [_, isFirstVisit] = seen.insert(type);
  if (!isFirstVisit) {
    return {};
  }
  auto parts = std::vector<const Type *>{type->tupleRest()};
  for (const Field &field : type->fields()) {
    parts.push_back(field.type);
  }

  return parts;
}

std::vector<const Type *> ConditionalTypes::plainParamsIn(const Type *type) const {
  auto params = std::vector<const Type *>();
  auto seen = llvm::SmallPtrSet<const Type *, 8>();
  auto stack = std::vector<const Type *>{type};
  while (!stack.empty()) {
    const Type *next = stack.back();
    stack.pop_back();
    bool isFirst = this->isFirstMention(next, seen);
    if (isFirst && this->isPlainParam(next)) {
      params.push_back(next);
    } else if (isFirst) {
      auto parts = this->paramParts(next);
      stack.insert(stack.end(), parts.rbegin(), parts.rend());
    }
  }

  return params;
}

bool ConditionalTypes::isFirstMention(const Type *type,
                                      llvm::SmallPtrSetImpl<const Type *> &seen) const {
  if (type == nullptr || !TypeQueries::mentionsParam(type)) {
    return false;
  }
  auto [_, isFirstVisit] = seen.insert(type);

  return isFirstVisit;
}

std::vector<const Type *> ConditionalTypes::paramParts(const Type *type) const {
  switch (type->kind()) {
    case Type::Kind::Optional:
    case Type::Kind::Array:
    case Type::Kind::Promise:
      return {type->inner()};
    case Type::Kind::Result:
      return {type->okType(), type->errType()};
    case Type::Kind::Function:
      return this->functionParts(type);
    case Type::Kind::Dict:
      return {type->keyType(), type->valueType()};
    case Type::Kind::Union:
      return std::vector<const Type *>(type->members().begin(), type->members().end());
    case Type::Kind::Object:
      return this->objectParts(type);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Bool:
    case Type::Kind::Int:
    case Type::Kind::Double:
    case Type::Kind::Decimal:
    case Type::Kind::String:
    case Type::Kind::Param:
    case Type::Kind::Never:
    case Type::Kind::Iterator:
    case Type::Kind::Json:
    case Type::Kind::Literal:
    case Type::Kind::Unknown:
    case Type::Kind::ClassRef:
      return {};
  }

  return {};
}

std::vector<const Type *> ConditionalTypes::objectParts(const Type *object) const {
  auto parts = std::vector<const Type *>(object->typeArgs().begin(), object->typeArgs().end());
  for (const Field &field : object->fields()) {
    parts.push_back(field.type);
  }

  return parts;
}

void ConditionalTypes::bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc) {
  auto *alias = this->state.getAst().make<TypeAliasDecl>(loc, name, TypeRef());
  alias->resolved = type;
  this->state.getScopeStack().setType(name, alias);
}

const Type *ConditionalTypes::resolveTypeArgument(const TypeRef &ref) {
  return this->typeResolver->resolveTypeInner(ref);
}

const Type *ConditionalTypes::resolveConditional(const TypeRef &ref) {
  bool isTail = this->state.getTypeContext().isTailCallAllowed();
  this->state.getTypeContext().forbidTailCall();
  auto preparation = this->prepareConditional(ref);
  const AnsweredConditional *answered = preparation.as<AnsweredConditional>();
  if (answered != nullptr) {
    return answered->answer;
  }
  const PreparedConditional &prepared = *preparation.as<PreparedConditional>();
  if (this->isDistributing(ref)) {
    auto members = TypeopsCheckHelpers::distributedMembers(prepared.check);
    if (members.size() != 1) {
      return this->distributeConditional(ref, members);
    }
  }
  auto call = isTail ? BranchCall::TAIL : BranchCall::PLAIN;

  return this->chooseBranch(ref, prepared, call);
}

bool ConditionalTypes::isDistributing(const TypeRef &ref) const {
  const TypeRef &checked = ref.args[0];
  if (checked.kind() != TypeRefKind::NAME_REF) {
    return false;
  }

  return this->isBinding(this->typeDeclarer->lookupType(checked.name));
}

const Type *ConditionalTypes::distributeConditional(const TypeRef &ref,
                                                    llvm::ArrayRef<const Type *> members) {
  auto results = std::vector<const Type *>();
  for (const Type *member : members) {
    this->state.getScopeStack().openTypeScope();
    this->bindType(ref.args[0].name, member, ref.loc);
    const auto *result = this->memberConditional(ref);
    this->state.getScopeStack().closeTypeScope();
    if (result->isError()) {
      return result;
    }
    results.push_back(result);
  }
  if (results.empty()) {
    return Type::never();
  }

  return Type::unionOf(results);
}

const Type *ConditionalTypes::memberConditional(const TypeRef &ref) {
  this->state.getTypeContext().forbidTailCall();
  auto preparation = this->prepareConditional(ref);
  const AnsweredConditional *answered = preparation.as<AnsweredConditional>();
  if (answered != nullptr) {
    return answered->answer;
  }

  return this->chooseBranch(ref, *preparation.as<PreparedConditional>(), BranchCall::PLAIN);
}

ConditionalPreparation ConditionalTypes::prepareConditional(const TypeRef &ref) {
  if (ref.args.size() != 4) {
    return AnsweredConditional{.answer = Type::error()};
  }
  const auto *check = this->typeResolver->resolveTypeInner(ref.args[0]);
  if (check->isError()) {
    return AnsweredConditional{.answer = check};
  }
  auto inferRefs = this->infersIn(ref.args[1]);
  this->state.getScopeStack().openTypeScope();
  auto infers = this->bindInferParams(inferRefs);
  const Type *failed = this->constrainInfers(inferRefs, infers);
  if (failed != nullptr) {
    this->state.getScopeStack().closeTypeScope();

    return AnsweredConditional{.answer = failed};
  }
  const Type *pattern = this->resolvePattern(ref.args[1]);
  this->state.getScopeStack().closeTypeScope();
  if (pattern->isError()) {
    return AnsweredConditional{.answer = pattern};
  }
  if (TypeQueries::mentionsParam(check) || this->isMentioningParamBesides(pattern, infers)) {
    return AnsweredConditional{.answer = this->deferConditional(ref)};
  }

  return PreparedConditional{
      .check = check,
      .pattern = pattern,
      .inferRefs = std::move(inferRefs),
      .infers = std::move(infers),
  };
}

std::vector<const Type *> ConditionalTypes::bindInferParams(llvm::ArrayRef<InferRef> inferRefs) {
  auto infers = std::vector<const Type *>();
  for (const InferRef &infer : inferRefs) {
    auto *param = Type::param(infer.ref->name);
    if (infer.isList) {
      param->setList();
    }
    infers.push_back(param);
    this->bindType(infer.ref->name, param, infer.ref->loc);
  }

  return infers;
}

const Type *ConditionalTypes::constrainInfers(llvm::ArrayRef<InferRef> inferRefs,
                                              llvm::ArrayRef<const Type *> infers) {
  for (size_t index = 0; index < inferRefs.size(); ++index) {
    const Type *constraint = this->writtenConstraint(*inferRefs[index].ref);
    if (constraint != nullptr && constraint->isError()) {
      return constraint;
    }
    if (constraint != nullptr) {
      const_cast<Type *>(infers[index])->setConstraint(constraint, {});
    }
  }

  return nullptr;
}

const Type *ConditionalTypes::writtenConstraint(const TypeRef &infer) {
  if (infer.args.empty()) {
    return nullptr;
  }

  return this->typeResolver->resolveTypeInner(infer.args[0]);
}

const Type *ConditionalTypes::resolvePattern(const TypeRef &patternRef) {
  this->state.getTypeContext().enterExtendsClause();
  const auto *pattern = this->typeResolver->resolveTypeInner(patternRef);
  this->state.getTypeContext().leaveExtendsClause();

  return pattern;
}

const Type *ConditionalTypes::chooseBranch(const TypeRef &ref, const PreparedConditional &prepared,
                                           BranchCall call) {
  auto unbound = std::vector<const Type *>(prepared.infers.size(), nullptr);
  auto inference = InferMatch{.params = prepared.infers, .bound = unbound};
  bool isYes = this->extendsRelation->isExtendingType(prepared.check, prepared.pattern, &inference);
  this->state.getScopeStack().openTypeScope();
  if (isYes) {
    this->bindInferred(prepared, inference);
  }
  const TypeRef &branch = isYes ? ref.args[2] : ref.args[3];
  const Type *result = this->resolveBranch(branch, call);
  this->state.getScopeStack().closeTypeScope();

  return result;
}

void ConditionalTypes::bindInferred(const PreparedConditional &prepared,
                                    const InferMatch &inference) {
  for (size_t index = 0; index < prepared.infers.size(); ++index) {
    const TypeRef *infer = prepared.inferRefs[index].ref;
    const Type *bound = this->boundOrDefault(inference.bound[index], prepared.infers[index]);
    this->bindType(infer->name, bound, infer->loc);
  }
}

const Type *ConditionalTypes::boundOrDefault(const Type *bound, const Type *infer) const {
  if (bound != nullptr) {
    return bound;
  }
  if (infer->constraint() != nullptr) {
    return infer->constraint();
  }

  return Type::unknown();
}

const Type *ConditionalTypes::resolveBranch(const TypeRef &branch, BranchCall call) {
  if (call == BranchCall::PLAIN) {
    return this->typeResolver->resolveTypeInner(branch);
  }
  if (branch.kind() == TypeRefKind::CONDITIONAL_REF) {
    this->state.getTypeContext().allowTailCall();

    return this->typeResolver->resolveTypeInner(branch);
  }
  TypeAliasDecl *alias = this->genericAliasNamed(branch);
  if (alias == nullptr) {
    return this->typeResolver->resolveTypeInner(branch);
  }

  return this->rememberTailCall(alias, branch);
}

TypeAliasDecl *ConditionalTypes::genericAliasNamed(const TypeRef &branch) const {
  if (branch.kind() != TypeRefKind::GENERIC_REF) {
    return nullptr;
  }
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(branch.name));
  if (alias == nullptr || alias->typeParams.empty()) {
    return nullptr;
  }

  return alias;
}

const Type *ConditionalTypes::rememberTailCall(TypeAliasDecl *alias, const TypeRef &branch) {
  auto args = std::vector<const Type *>();
  for (const TypeRef &arg : branch.args) {
    const Type *type = this->resolveTypeArgument(arg);
    if (type->isError()) {
      return type;
    }
    args.push_back(type);
  }
  this->state.getTypeContext().rememberTailCall(alias, std::move(args));

  return nullptr;
}

const Type *ConditionalTypes::deferConditional(const TypeRef &ref) {
  const ConditionalInfo *known = this->knownConditional(ref);
  auto operands = std::vector<const Type *>{known->tag};
  for (llvm::StringRef name : known->bound) {
    operands.push_back(this->boundType(name));
  }

  return Type::typeOp(TypeOp::Conditional, operands);
}

const ConditionalInfo *ConditionalTypes::knownConditional(const TypeRef &ref) {
  const auto *known = this->state.getTypeContext().findConditional(&ref);
  if (known != nullptr) {
    return known;
  }
  auto info = ConditionalInfo();
  info.ref = &ref;
  info.tag = Type::param(this->spellingOf(ref));
  for (llvm::StringRef name : this->namesIn(ref)) {
    auto *decl = this->typeDeclarer->lookupType(name);
    if (decl != nullptr && this->isBinding(decl)) {
      info.bound.push_back(name);
    } else if (decl != nullptr) {
      info.decls.push_back({name, decl});
    }
  }

  return this->state.getTypeContext().addConditional(std::move(info));
}

std::string ConditionalTypes::spellingOf(const TypeRef &ref) const {
  auto spelling = std::string();
  for (char unit : ref.source) {
    bool isSpace = unit == ' ' || unit == '\n' || unit == '\r' || unit == '\t';
    bool isAfterSpace = spelling.empty() || spelling.back() == ' ';
    if (isSpace && !isAfterSpace) {
      spelling += ' ';
    }
    if (!isSpace) {
      spelling += unit;
    }
  }
  while (!spelling.empty() && spelling.back() == ' ') {
    spelling.pop_back();
  }
  if (spelling.empty()) {
    return kUnnamedConditional;
  }

  return spelling;
}

const Type *ConditionalTypes::boundType(llvm::StringRef name) const {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(name));
  if (alias == nullptr || alias->resolved == nullptr) {
    return Type::error();
  }

  return alias->resolved;
}

void ConditionalTypes::enterConditionalScope(const ConditionalInfo &info,
                                             llvm::ArrayRef<const Type *> operands) {
  this->state.getScopeStack().openTypeScope();
  for (const auto &[name, decl] : info.decls) {
    this->state.getScopeStack().setType(name, decl);
  }
  for (size_t index = 0; index < info.bound.size() && index + 1 < operands.size(); ++index) {
    this->bindType(info.bound[index], operands[index + 1], info.ref->loc);
  }
}

const Type *ConditionalTypes::evalConditional(llvm::ArrayRef<const Type *> operands) {
  const auto *info = this->state.getTypeContext().findConditionalByTag(operands[0]);
  if (info == nullptr) {
    return Type::error();
  }
  for (const Type *type : operands.drop_front()) {
    if (type->isError()) {
      return type;
    }
  }
  this->enterConditionalScope(*info, operands);
  auto &types = this->state.getTypeContext();
  bool isTail = types.isTailCallAllowed();
  types.forbidTailCall();
  const auto *result = this->resolveConditional(*info->ref);
  if (isTail) {
    types.allowTailCall();
  } else {
    types.forbidTailCall();
  }
  this->state.getScopeStack().closeTypeScope();
  if (result == nullptr) {
    return Type::error();
  }

  return result;
}

std::vector<const Type *> ConditionalTypes::conditionalBranches(const Type *conditional) {
  if (conditional->typeOp() != TypeOp::Conditional) {
    return {};
  }
  llvm::ArrayRef<const Type *> operands = conditional->operands();
  const auto *info = this->state.getTypeContext().findConditionalByTag(operands[0]);
  if (info == nullptr) {
    return {};
  }
  this->enterConditionalScope(*info, operands);
  auto inferRefs = this->infersIn(info->ref->args[1]);
  this->bindInferParams(inferRefs);
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  auto branches = std::vector<const Type *>();
  for (size_t index : {2, 3}) {
    const auto *branch = this->typeResolver->resolveTypeInner(info->ref->args[index]);
    if (branch != nullptr && !branch->isError()) {
      branches.push_back(branch);
    }
  }
  this->state.getDiagnostics().popList();
  this->state.getScopeStack().closeTypeScope();

  return branches;
}

bool ConditionalTypes::isFittingConditional(const Type *from, const Type *to) {
  auto first = FitsGoal{.from = from, .to = to, .depth = 0};
  auto steps = std::vector<FitsStep>{first};
  while (!steps.empty()) {
    FitsStep step = std::move(steps.back());
    steps.pop_back();
    const FitsGoal *goal = step.as<FitsGoal>();
    bool isHolding = goal == nullptr || this->isGoalFitting(*goal, steps);
    if (!isHolding && !this->canRetryFits(steps)) {
      return false;
    }
  }

  return true;
}

bool ConditionalTypes::isGoalFitting(const FitsGoal &goal, std::vector<FitsStep> &steps) {
  if (goal.depth > kMaxFitsDepth || goal.to->typeOp() != TypeOp::Conditional) {
    return false;
  }
  if (goal.from->isUnion()) {
    for (const Type *member : llvm::reverse(goal.from->members())) {
      steps.push_back(FitsGoal{.from = member, .to = goal.to, .depth = goal.depth + 1});
    }
    return true;
  }
  auto branches = this->conditionalBranches(goal.to);
  auto choice = FitsChoice{
      .from = goal.from,
      .depth = goal.depth,
      .branches = std::move(branches),
      .next = 0,
  };

  return this->canTryFitsAlternative(std::move(choice), steps);
}

bool ConditionalTypes::canRetryFits(std::vector<FitsStep> &steps) {
  while (!steps.empty()) {
    FitsStep step = std::move(steps.back());
    steps.pop_back();
    FitsChoice *choice = step.as<FitsChoice>();
    if (choice != nullptr && this->canTryFitsAlternative(std::move(*choice), steps)) {
      return true;
    }
  }

  return false;
}

bool ConditionalTypes::canTryFitsAlternative(FitsChoice choice, std::vector<FitsStep> &steps) {
  while (choice.next < choice.branches.size() * kFitsWayCount) {
    const Type *branch = choice.branches[choice.next / kFitsWayCount];
    auto way = FitsWay(choice.next % kFitsWayCount);
    choice.next += 1;
    if (way == FitsWay::NESTED) {
      auto goal = FitsGoal{.from = choice.from, .to = branch, .depth = choice.depth + 1};
      steps.push_back(std::move(choice));
      steps.push_back(goal);
      return true;
    }
    if (this->isFittingBy(way, choice.from, branch)) {
      return true;
    }
  }

  return false;
}

bool ConditionalTypes::isFittingBy(FitsWay way, const Type *from, const Type *branch) {
  switch (way) {
    case FitsWay::CONVERSION:
      return TypeRelations::isImplicitlyConvertible(from, branch);
    case FitsWay::NESTED:
      return false;
    case FitsWay::INFERENCE:
      return this->isExtendingWithParams(from, branch);
  }

  return false;
}

bool ConditionalTypes::isExtendingWithParams(const Type *from, const Type *branch) {
  auto params = this->plainParamsIn(branch);
  if (params.empty()) {
    return false;
  }
  auto unbound = std::vector<const Type *>(params.size(), nullptr);
  auto match = InferMatch{.params = params, .bound = unbound};

  return this->extendsRelation->isExtendingType(from, branch, &match);
}

ConditionalTypes::ResultIntersectionJoined
ConditionalTypes::resolveIntersection(const TypeRef &ref) {
  auto parts = this->intersectionParts(ref);
  if (!parts.empty() && parts.back()->isError()) {
    return ok(parts.back());
  }
  const Type *narrowed = this->narrowedByIntersection(parts);
  if (narrowed != nullptr) {
    return ok(narrowed);
  }
  auto objects = this->checkAllObjects(parts);
  if (objects.isErr()) {
    return err(objects.error());
  }
  auto merged = this->fieldCopies(parts.front());
  for (const Type *part : llvm::ArrayRef<const Type *>(parts).drop_front()) {
    auto next = this->mergedWith(merged, part);
    if (next.isErr()) {
      return err(next.error());
    }
    merged = next.value();
  }
  const auto *joined = Type::unionOf(merged);

  return ok(joined);
}

std::vector<const Type *> ConditionalTypes::intersectionParts(const TypeRef &ref) {
  auto parts = std::vector<const Type *>();
  for (const TypeRef &arg : ref.args) {
    const auto *part = this->typeResolver->resolveTypeInner(arg);
    parts.push_back(part);
    if (part->isError()) {
      break;
    }
  }

  return parts;
}

const Type *ConditionalTypes::narrowedByIntersection(llvm::ArrayRef<const Type *> parts) {
  if (parts.size() != 2) {
    return nullptr;
  }
  for (size_t index : {0, 1}) {
    const Type *narrowed = this->narrowedBy(parts[index], parts[1 - index]);
    if (narrowed != nullptr) {
      return narrowed;
    }
  }

  return nullptr;
}

const Type *ConditionalTypes::narrowedBy(const Type *filter, const Type *other) {
  if (this->isPrimitive(filter)) {
    const auto *unevaluated = Type::typeOp(TypeOp::Extract, {other, filter});

    return this->typeOperators->evalTypeOp(unevaluated, {other, filter});
  }
  if (this->isEmptyObject(filter)) {
    const auto *unevaluated = Type::typeOp(TypeOp::NonNullable, {other});

    return this->typeOperators->evalTypeOp(unevaluated, {other});
  }

  return nullptr;
}

bool ConditionalTypes::isPrimitive(const Type *type) const {
  return (type->isString() && !type->isPattern()) || type->isNumeric() || type->isBool();
}

bool ConditionalTypes::isPlainObject(const Type *type) const {
  return type->isObject() && !type->isClass() && !type->isTuple() && !type->isAnyObject() &&
         type->overloads().empty();
}

bool ConditionalTypes::isEmptyObject(const Type *type) const {
  return this->isPlainObject(type) && type->fields().empty();
}

ConditionalTypes::ResultObjectsAgreed
ConditionalTypes::checkAllObjects(llvm::ArrayRef<const Type *> parts) const {
  const Type *other = this->firstNonObject(parts);
  if (other != nullptr) {
    auto message = ErrorTextIntersection::notObject(other->name());

    return err(ConditionalTypesResolveIntersectionError{
        .code = ConditionalTypesResolveIntersectionErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok();
}

std::vector<const Type *> ConditionalTypes::fieldCopies(const Type *part) const {
  auto copies = std::vector<const Type *>();
  for (const Type *member : this->membersOf(part)) {
    auto fields = std::vector<Field>(member->fields().begin(), member->fields().end());
    copies.push_back(Type::object(std::move(fields)));
  }

  return copies;
}

const Type *ConditionalTypes::firstNonObject(llvm::ArrayRef<const Type *> parts) const {
  for (const Type *part : parts) {
    for (const Type *member : this->membersOf(part)) {
      if (!this->isPlainObject(member)) {
        return member;
      }
    }
  }

  return nullptr;
}

ConditionalTypes::ResultObjectsMerged
ConditionalTypes::mergedWith(llvm::ArrayRef<const Type *> merged, const Type *part) const {
  auto next = std::vector<const Type *>();
  for (const Type *left : merged) {
    for (const Type *right : this->membersOf(part)) {
      auto joined = this->joinObjects(left, right);
      if (joined.isErr()) {
        return err(joined.error());
      }
      next.push_back(joined.value());
    }
  }

  return ok(next);
}

ConditionalTypes::ResultIntersectionJoined ConditionalTypes::joinObjects(const Type *left,
                                                                         const Type *right) const {
  auto fields = std::vector<Field>(left->fields().begin(), left->fields().end());
  for (const Field &field : right->fields()) {
    auto added = this->joinField(fields, field);
    if (added.isErr()) {
      return err(added.error());
    }
  }
  const auto *object = Type::object(std::move(fields));

  return ok(object);
}

ConditionalTypes::ResultObjectsAgreed ConditionalTypes::joinField(std::vector<Field> &fields,
                                                                  const Field &field) const {
  Field *same = this->fieldNamed(fields, field.name);
  if (same == nullptr) {
    fields.push_back(field);

    return ok();
  }
  const Type *first = same->type->nonOptional();
  const Type *second = field.type->nonOptional();
  const Type *both = this->commonFieldType(first, second);
  if (both == nullptr) {
    auto message = ErrorTextIntersection::fieldTypes(field.name, first->name(), second->name());

    return err(ConditionalTypesResolveIntersectionError{
        .code = ConditionalTypesResolveIntersectionErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }
  bool isOptional = same->type->isOptional() && field.type->isOptional();
  same->type = isOptional ? Type::optional(both) : both;
  same->readonly |= field.readonly;

  return ok();
}

Field *ConditionalTypes::fieldNamed(std::vector<Field> &fields, llvm::StringRef name) const {
  for (Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

const Type *ConditionalTypes::commonFieldType(const Type *first, const Type *second) const {
  if (first == second) {
    return first;
  }
  if (first->isLiteral() && first->literalBase() == second) {
    return first;
  }
  if (second->isLiteral() && second->literalBase() == first) {
    return second;
  }

  return nullptr;
}

const Type *ConditionalTypes::signatureForUtility(const TypeRef &ref) {
  if (ref.kind() != TypeRefKind::TYPE_OF_REF || ref.names().empty()) {
    return nullptr;
  }
  if (ref.names().size() == 1) {
    return this->genericFunctionSignature(ref.names().front());
  }
  if (ref.names().size() == 2) {
    return this->methodSignature(ref.names()[0], ref.names()[1]);
  }

  return nullptr;
}

const Type *ConditionalTypes::genericFunctionSignature(llvm::StringRef name) {
  auto entry = this->topLevelChecker->lookupValue(name);
  FuncSymbol *function = entry.func();
  if (function == nullptr || !function->isGeneric()) {
    return nullptr;
  }

  return this->erasedSignature(function);
}

const Type *ConditionalTypes::methodSignature(llvm::StringRef objectName,
                                              llvm::StringRef methodName) {
  auto entry = this->topLevelChecker->lookupValue(objectName);
  VarSymbol *variable = entry.var();
  if (variable == nullptr || variable->type == nullptr) {
    return nullptr;
  }
  const Type *object = variable->type->nonOptional();
  const Overload *overload = object->isObject() ? object->overload(methodName) : nullptr;
  if (overload != nullptr) {
    return overload->signatures.back();
  }

  return this->classMethodSignature(object, methodName);
}

const Type *ConditionalTypes::classMethodSignature(const Type *object, llvm::StringRef methodName) {
  auto *classDecl = this->classDeclarer->classOf(object);
  if (classDecl == nullptr || object->field(methodName) != nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  auto methods = this->classDeclarer->methodsNamed(classDecl, methodName);
  if (methods.empty() || methods.back()->sym == nullptr) {
    return nullptr;
  }

  return this->erasedSignature(methods.back()->sym);
}

const Type *ConditionalTypes::erasedSignature(FuncSymbol *function) {
  const auto *type = this->returnInferrer->functionType(function);
  auto args = std::vector<const Type *>();
  for (const Type *param : function->typeParams) {
    args.push_back(this->erasedParam(param));
  }

  return this->typeParams->substitute(type, function->typeParams, args);
}

const Type *ConditionalTypes::erasedParam(const Type *param) const {
  if (param->constraint() != nullptr) {
    return param->constraint();
  }
  if (!param->alternatives().empty()) {
    return Type::unionOf(param->alternatives());
  }
  if (param->isListParam()) {
    return Type::array(Type::unknown());
  }

  return Type::unknown();
}

}
