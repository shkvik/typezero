#include "compiler/semantic/types_v2/conditionals/types_conditionals_registry.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cmath>

namespace tz {

namespace {

constexpr unsigned kMaxExpectationDepth = 8;
constexpr const char *kUnnamedConditional = "conditional type";

}

TypesConditionalsRegistry::TypesConditionalsRegistry(SemanticState &owner)
    : state(owner) {}

bool TypesConditionalsRegistry::isBinding(const Stmt *decl) const {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(decl);

  return alias != nullptr && alias->resolved != nullptr && alias->typeParams.empty() &&
         alias->type.kind() == TypeRefKind::NAME_REF && alias->type.name.empty();
}

std::vector<llvm::StringRef> TypesConditionalsRegistry::namesIn(const TypeRef &ref) const {
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

std::vector<InferRef> TypesConditionalsRegistry::infersIn(const TypeRef &ref) const {
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

void TypesConditionalsRegistry::noteInfer(SpreadRef part, std::vector<InferRef> &infers) const {
  for (InferRef &known : infers) {
    if (known.ref->name == part.ref->name) {
      known.isList = known.isList || part.isSpread;
      return;
    }
  }
  infers.push_back(InferRef{.ref = part.ref, .isList = part.isSpread});
}

void TypesConditionalsRegistry::pushSpreadArgs(const TypeRef &ref,
                                               std::vector<SpreadRef> &stack) const {
  for (size_t index = ref.args.size(); index > 0; --index) {
    bool isSpread = this->isSpreadAt(ref, index - 1);
    stack.push_back(SpreadRef{.ref = &ref.args[index - 1], .isSpread = isSpread});
  }
}

bool TypesConditionalsRegistry::isSpreadAt(const TypeRef &ref, size_t index) const {
  bool isTupleSpread =
      ref.kind() == TypeRefKind::TUPLE_REF && index < ref.spread().size() && ref.spread()[index];
  bool isRestParam =
      ref.kind() == TypeRefKind::FUNCTION_REF && ref.rest() && index + 2 == ref.args.size();

  return isTupleSpread || isRestParam;
}

void TypesConditionalsRegistry::bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc) {
  auto *alias = this->state.ast.make<TypeAliasDecl>(loc, name, TypeRef());
  alias->resolved = type;
  this->state.walk.scopes.setType(name, alias);
}

bool TypesConditionalsRegistry::isDistributing(const TypeRef &ref) const {
  const TypeRef &checked = ref.args[0];
  if (checked.kind() != TypeRefKind::NAME_REF) {
    return false;
  }

  return this->isBinding(this->state.findTypeDeclaration(checked.name));
}

std::vector<const Type *>
TypesConditionalsRegistry::bindInferParams(llvm::ArrayRef<InferRef> inferRefs) {
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

const Type *TypesConditionalsRegistry::boundOrDefault(const Type *bound, const Type *infer) const {
  if (bound != nullptr) {
    return bound;
  }
  if (infer->constraint() != nullptr) {
    return infer->constraint();
  }

  return Type::unknown();
}

TypeAliasDecl *TypesConditionalsRegistry::genericAliasNamed(const TypeRef &branch) const {
  if (branch.kind() != TypeRefKind::GENERIC_REF) {
    return nullptr;
  }
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(this->state.findTypeDeclaration(branch.name));
  if (alias == nullptr || alias->typeParams.empty()) {
    return nullptr;
  }

  return alias;
}

const Type *TypesConditionalsRegistry::deferConditional(const TypeRef &ref) {
  const ConditionalInfo *known = this->knownConditional(ref);
  auto operands = std::vector<const Type *>{known->tag};
  for (llvm::StringRef name : known->bound) {
    operands.push_back(this->boundType(name));
  }

  return Type::typeOp(TypeOp::Conditional, operands);
}

const ConditionalInfo *TypesConditionalsRegistry::knownConditional(const TypeRef &ref) {
  const auto *known = this->state.types.context.findConditional(&ref);
  if (known != nullptr) {
    return known;
  }
  auto info = ConditionalInfo();
  info.ref = &ref;
  info.tag = Type::param(this->spellingOf(ref));
  for (llvm::StringRef name : this->namesIn(ref)) {
    auto *decl = this->state.findTypeDeclaration(name);
    if (decl != nullptr && this->isBinding(decl)) {
      info.bound.push_back(name);
    } else if (decl != nullptr) {
      info.decls.push_back({name, decl});
    }
  }

  return this->state.types.context.addConditional(std::move(info));
}

std::string TypesConditionalsRegistry::spellingOf(const TypeRef &ref) const {
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

const Type *TypesConditionalsRegistry::boundType(llvm::StringRef name) const {
  auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(this->state.findTypeDeclaration(name));
  if (alias == nullptr || alias->resolved == nullptr) {
    return Type::error();
  }

  return alias->resolved;
}

void TypesConditionalsRegistry::enterConditionalScope(const ConditionalInfo &info,
                                                      llvm::ArrayRef<const Type *> operands) {
  this->state.walk.scopes.openTypeScope();
  for (const auto &[name, decl] : info.decls) {
    this->state.walk.scopes.setType(name, decl);
  }
  for (size_t index = 0; index < info.bound.size() && index + 1 < operands.size(); ++index) {
    this->bindType(info.bound[index], operands[index + 1], info.ref->loc);
  }
}

void TypesConditionalsRegistry::startConditional(const StartConditional &task) {
  const TypeRef &ref = *task.ref;
  auto &types = this->state.types.context;
  bool isTail = !task.isMember && types.isTailCallAllowed();
  types.forbidTailCall();
  if (ref.args.size() != 4) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  auto draft = ConditionalDraft{
      .ref = &ref,
      .isMember = task.isMember,
      .isTail = isTail,
      .check = nullptr,
      .pattern = nullptr,
      .inferRefs = {},
      .infers = {},
      .next = 0,
  };
  this->state.walk.tasks.pushTask(AfterConditionalCheck{.draft = draft});
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::INNER});
}

void TypesConditionalsRegistry::afterConditionalCheck(const AfterConditionalCheck &task) {
  ConditionalDraft draft = task.draft;
  const Type *check = this->state.walk.tasks.takeAnswer();
  if (check->isError()) {
    return this->state.walk.tasks.pushAnswer(check);
  }
  draft.check = check;
  draft.inferRefs = this->infersIn(draft.ref->args[1]);
  this->state.walk.scopes.openTypeScope();
  draft.infers = this->bindInferParams(draft.inferRefs);

  return this->nextInferConstraint(draft);
}

void TypesConditionalsRegistry::nextInferConstraint(ConditionalDraft draft) {
  while (draft.next < draft.inferRefs.size() && draft.inferRefs[draft.next].ref->args.empty()) {
    draft.next += 1;
  }
  if (draft.next < draft.inferRefs.size()) {
    const TypeRef &constraint = draft.inferRefs[draft.next].ref->args[0];
    this->state.walk.tasks.pushTask(AfterInferConstraint{.draft = draft});

    return this->state.walk.tasks.pushTask(
        ResolveTypeAnswer{.ref = &constraint, .use = TypeUse::INNER});
  }
  this->state.types.context.enterExtendsClause();
  this->state.walk.tasks.pushTask(AfterConditionalPattern{.draft = draft});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &draft.ref->args[1], .use = TypeUse::INNER});
}

void TypesConditionalsRegistry::afterInferConstraint(const AfterInferConstraint &task) {
  ConditionalDraft draft = task.draft;
  const Type *constraint = this->state.walk.tasks.takeAnswer();
  if (constraint->isError()) {
    this->state.walk.scopes.closeTypeScope();

    return this->state.walk.tasks.pushAnswer(constraint);
  }
  const_cast<Type *>(draft.infers[draft.next])->setConstraint(constraint, {});
  draft.next += 1;

  return this->nextInferConstraint(draft);
}

void TypesConditionalsRegistry::afterConditionalPattern(const AfterConditionalPattern &task) {
  ConditionalDraft draft = task.draft;
  const TypeRef &ref = *draft.ref;
  this->state.types.context.leaveExtendsClause();
  const Type *pattern = this->state.walk.tasks.takeAnswer();
  this->state.walk.scopes.closeTypeScope();
  if (pattern->isError()) {
    return this->state.walk.tasks.pushAnswer(pattern);
  }
  bool isOpen = TypeQueries::mentionsParam(draft.check) ||
                this->state.types.relations.isMentioningParamBesides(pattern, draft.infers);
  if (isOpen) {
    const auto *deferred = this->deferConditional(ref);

    return this->state.walk.tasks.pushAnswer(deferred);
  }
  draft.pattern = pattern;
  if (!draft.isMember && this->isDistributing(ref)) {
    auto members = TypeopsCheckHelpers::distributedMembers(draft.check);
    if (members.size() != 1) {
      return this->startDistribution(ref, members);
    }
  }
  BranchCall call = draft.isTail ? BranchCall::TAIL : BranchCall::PLAIN;

  return this->startBranchChoice(draft, call);
}

void TypesConditionalsRegistry::startBranchChoice(const ConditionalDraft &draft, BranchCall call) {
  auto unbound = std::vector<const Type *>(draft.infers.size(), nullptr);
  auto inference = InferMatch{.params = draft.infers, .bound = unbound};
  auto &relations = this->state.types.relations;
  auto needs = relations.findExtendsNeeds(draft.check, draft.pattern, &inference);
  this->state.walk.tasks.pushTask(ChooseBranch{.draft = draft, .call = call});
  for (FuncSymbol *function : llvm::reverse(needs)) {
    this->state.walk.tasks.pushTask(InferReturnType{.function = function});
  }
}

void TypesConditionalsRegistry::chooseBranch(const ChooseBranch &task) {
  const ConditionalDraft &draft = task.draft;
  const TypeRef &ref = *draft.ref;
  auto unbound = std::vector<const Type *>(draft.infers.size(), nullptr);
  auto inference = InferMatch{.params = draft.infers, .bound = unbound};
  bool isYes = this->state.types.relations.isExtendingType(draft.check, draft.pattern, &inference);
  this->state.walk.scopes.openTypeScope();
  if (isYes) {
    this->bindInferred(draft, inference);
  }
  const TypeRef &branch = isYes ? ref.args[2] : ref.args[3];
  this->state.walk.tasks.pushTask(CloseConditionalScope{});

  return this->pushBranch(branch, task.call);
}

void TypesConditionalsRegistry::closeConditionalScope() {
  this->state.walk.scopes.closeTypeScope();
}

void TypesConditionalsRegistry::pushBranch(const TypeRef &branch, BranchCall call) {
  TypeAliasDecl *alias = call == BranchCall::TAIL ? this->genericAliasNamed(branch) : nullptr;
  if (call == BranchCall::TAIL && branch.kind() == TypeRefKind::CONDITIONAL_REF) {
    this->state.types.context.allowTailCall();
  }
  if (alias == nullptr || branch.kind() == TypeRefKind::CONDITIONAL_REF) {
    return this->state.walk.tasks.pushTask(
        ResolveTypeAnswer{.ref = &branch, .use = TypeUse::INNER});
  }
  auto arguments = TypeArgumentsDraft{
      .ref = &branch,
      .decl = alias,
      .use = TypeArgumentsUse::TAIL_CALL,
      .next = 0,
      .args = {},
  };
  this->state.walk.tasks.pushTask(NextTypeArgument{.draft = arguments});
}

void TypesConditionalsRegistry::startDistribution(const TypeRef &ref,
                                                  std::vector<const Type *> members) {
  auto draft = DistributionDraft{
      .ref = &ref,
      .members = std::move(members),
      .next = 0,
      .results = {},
  };

  return this->nextDistributedMember(draft);
}

void TypesConditionalsRegistry::nextDistributedMember(const DistributionDraft &draft) {
  const TypeRef &ref = *draft.ref;
  if (draft.next == draft.members.size()) {
    const Type *joined = draft.results.empty() ? Type::never() : Type::unionOf(draft.results);

    return this->state.walk.tasks.pushAnswer(joined);
  }
  this->state.walk.scopes.openTypeScope();
  this->bindType(ref.args[0].name, draft.members[draft.next], ref.loc);
  this->state.walk.tasks.pushTask(AfterDistributedMember{.draft = draft});
  this->state.walk.tasks.pushTask(StartConditional{.ref = &ref, .isMember = true});
}

void TypesConditionalsRegistry::afterDistributedMember(const AfterDistributedMember &task) {
  DistributionDraft draft = task.draft;
  const Type *result = this->state.walk.tasks.takeAnswer();
  this->state.walk.scopes.closeTypeScope();
  if (result->isError()) {
    return this->state.walk.tasks.pushAnswer(result);
  }
  draft.results.push_back(result);
  draft.next += 1;

  return this->nextDistributedMember(draft);
}

void TypesConditionalsRegistry::evalConditional(const EvalConditional &task) {
  const std::vector<const Type *> &operands = task.operands;
  auto &types = this->state.types.context;
  const auto *info = types.findConditionalByTag(operands[0]);
  if (info == nullptr) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  for (const Type *type : llvm::ArrayRef<const Type *>(operands).drop_front()) {
    if (type->isError()) {
      return this->state.walk.tasks.pushAnswer(type);
    }
  }
  this->enterConditionalScope(*info, operands);
  bool isTail = types.isTailCallAllowed();
  types.forbidTailCall();
  this->state.walk.tasks.pushTask(AfterEvalConditional{.isTail = isTail});
  this->state.walk.tasks.pushTask(StartConditional{.ref = info->ref, .isMember = false});
}

void TypesConditionalsRegistry::afterEvalConditional(const AfterEvalConditional &task) {
  auto &types = this->state.types.context;
  const Type *result = this->state.walk.tasks.takeAnswer();
  if (task.isTail) {
    types.allowTailCall();
  } else {
    types.forbidTailCall();
  }
  this->state.walk.scopes.closeTypeScope();
  const Type *answer = result != nullptr ? result : Type::error();
  this->state.walk.tasks.pushAnswer(answer);
}

void TypesConditionalsRegistry::evalFiltered(const EvalFiltered &task) {
  auto &relations = this->state.types.relations;
  auto needs = std::vector<FuncSymbol *>();
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(task.members)) {
    auto found = relations.findExtendsNeeds(member, task.filter, nullptr);
    needs.insert(needs.end(), found.begin(), found.end());
  }
  this->state.walk.tasks.pushTask(
      FinishFiltered{.kind = task.kind, .members = task.members, .filter = task.filter});
  for (FuncSymbol *function : llvm::reverse(needs)) {
    this->state.walk.tasks.pushTask(InferReturnType{.function = function});
  }
}

void TypesConditionalsRegistry::finishFiltered(const FinishFiltered &task) {
  bool isKeepingMatches = task.kind == TypeOp::Extract;
  auto kept = std::vector<const Type *>();
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(task.members)) {
    bool isMatching = this->state.types.relations.isExtendingType(member, task.filter);
    if (isMatching == isKeepingMatches) {
      kept.push_back(member);
    }
  }
  const Type *answer = kept.empty() ? Type::never() : Type::unionOf(kept);
  this->state.walk.tasks.pushAnswer(answer);
}

void TypesConditionalsRegistry::prepareExpectation(const PrepareExpectation &task) {
  const Type *expected = task.type;
  bool isConditional = expected != nullptr && task.depth <= kMaxExpectationDepth &&
                       expected->nonOptional()->typeOp() == TypeOp::Conditional;
  if (!isConditional) {
    return;
  }
  const Type *conditional = expected->nonOptional();
  auto &types = this->state.types.context;
  if (types.hasConditionalBranches(conditional)) {
    auto branches = types.findConditionalBranches(conditional);

    return this->pushExpectations(branches, task.depth + 1);
  }
  llvm::ArrayRef<const Type *> operands = conditional->operands();
  const auto *info = types.findConditionalByTag(operands[0]);
  if (info == nullptr) {
    return types.rememberConditionalBranches(conditional, {});
  }
  this->enterConditionalScope(*info, operands);
  auto inferRefs = this->infersIn(info->ref->args[1]);
  this->bindInferParams(inferRefs);
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  this->state.walk.tasks.pushTask(FinishBranches{.conditional = conditional, .depth = task.depth});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &info->ref->args[3], .use = TypeUse::INNER});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &info->ref->args[2], .use = TypeUse::INNER});
}

void TypesConditionalsRegistry::finishBranches(const FinishBranches &task) {
  const Type *no = this->state.walk.tasks.takeAnswer();
  const Type *yes = this->state.walk.tasks.takeAnswer();
  this->state.diagnostics.popList();
  this->state.walk.scopes.closeTypeScope();
  auto branches = std::vector<const Type *>();
  for (const Type *branch : {yes, no}) {
    if (branch != nullptr && !branch->isError()) {
      branches.push_back(branch);
    }
  }
  this->state.types.context.rememberConditionalBranches(task.conditional, branches);

  return this->pushExpectations(branches, task.depth + 1);
}

void TypesConditionalsRegistry::pushExpectations(llvm::ArrayRef<const Type *> branches,
                                                 unsigned depth) {
  for (const Type *branch : llvm::reverse(branches)) {
    this->state.walk.tasks.pushTask(PrepareExpectation{.type = branch, .depth = depth});
  }
}

void TypesConditionalsRegistry::bindInferred(const ConditionalDraft &draft,
                                             const InferMatch &inference) {
  for (size_t index = 0; index < draft.infers.size(); ++index) {
    const TypeRef *infer = draft.inferRefs[index].ref;
    const Type *bound = this->boundOrDefault(inference.bound[index], draft.infers[index]);
    this->bindType(infer->name, bound, infer->loc);
  }
}

}
