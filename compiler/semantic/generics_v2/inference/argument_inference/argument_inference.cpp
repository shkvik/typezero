#include "compiler/semantic/generics_v2/inference/argument_inference/argument_inference.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/types/argument_lists/argument_lists.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

const GlobalName kContextFunctions[] =
    {

    }

ArgumentInference::ArgumentInference(SemanticState & owner, BindingRules &sharedBindingRules)
    : state(owner),
                 bindingRules(sharedBindingRules) {}

void ArgumentInference::nextBindingArgument(const NextBindingArgument &task) {
  auto draft = task.draft;
  size_t index = draft.index;
  if (index == draft.call->args.size()) {
    return this->inferFromListRest(InferFromListRest{.draft = std::move(draft)});
  }
  auto form = this->argumentForm(draft, index);
  const auto *pattern = this->bindingRules.parameterFor(draft, index);
  draft.index = index + 1;
  switch (form.kind()) {
    case GenericArgumentKind::LIST_REST:
      return this->walkArgumentNow(std::move(draft), index, nullptr);
    case GenericArgumentKind::PLAIN:
      return this->walkArgumentNow(std::move(draft), index, pattern);
    case GenericArgumentKind::LITERAL_TEXT:
      return this->inferFromLiteralText(std::move(draft), index,
                                        form.as<LiteralTextArgument>()->text);
    case GenericArgumentKind::CONTEXTUAL:
      draft.contextual.push_back(index);
      return this->state.walk.tasks.pushTask(NextBindingArgument{.draft = std::move(draft)});
    case GenericArgumentKind::LITERAL_SHAPE:
      return this->inferFromLiteralShape(std::move(draft), index,
                                         form.as<LiteralShapeArgument>()->shape);
    case GenericArgumentKind::CONTEXTUAL_LITERAL:
      return this->deferLiteral(std::move(draft), index);
    case GenericArgumentKind::INFERRED:
      return this->inferFromCheckedArgument(std::move(draft), index);
  }
}

GenericArgumentForm ArgumentInference::argumentForm(const GenericBindingDraft &draft,
                                                    size_t index) {
  Expr *arg = draft.call->args[index];
  llvm::ArrayRef<const Type *> params = draft.generic->typeParams;
  const auto *pattern = this->bindingRules.parameterFor(draft, index);
  if (draft.listRest != nullptr && index >= draft.fixed) {
    return ListRestArgument{};
  }
  if (!GenericCheckHelpers::mentionsAnyOf(pattern, params)) {
    return PlainArgument{};
  }
  auto *text = dyn_cast<StringLitExpr>(arg);
  bool isLiteralText = text != nullptr && llvm::is_contained(params, pattern) &&
                       this->state.types.relations.isExpectingLiteral(pattern);
  if (isLiteralText) {
    return LiteralTextArgument{.text = text};
  }
  bool isContextual =
      this->isContextSensitive(arg) || this->isMakingResults(arg) || this->isBareGenericName(arg);
  if (isContextual) {
    return ContextualArgument{};
  }
  const auto *shape = this->bindingRules.literalShapeOf(arg);
  if (shape != nullptr) {
    return LiteralShapeArgument{.shape = shape};
  }
  if (isa<ArrayLitExpr>(arg) || isa<ObjectLitExpr>(arg)) {
    return ContextualLiteralArgument{};
  }

  return InferredArgument{};
}

bool ArgumentInference::isContextSensitive(const Expr *arg) const {
  for (const Expr *leaf : ExprParts::literalLeaves(arg)) {
    if (this->isNeedingContext(leaf)) {
      return true;
    }
  }

  return false;
}

bool ArgumentInference::isNeedingContext(const Expr *arg) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  auto *name = dyn_cast<NameExpr>(arg);
  auto *member = dyn_cast<MemberExpr>(arg);
  if (functionExpr != nullptr) {
    return this->hasUntypedParam(functionExpr->fn);
  }
  if (name != nullptr) {
    auto global = GlobalNameTable::find(name->name);
    bool isContextFunction = global.has_value() && llvm::is_contained(kContextFunctions, *global);

    return name->typeArgs.empty() && isContextFunction;
  }
  auto *object = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;
  if (object != nullptr) {
    auto global = GlobalNameTable::find(object->name);

    return global.has_value() && llvm::is_contained(kContextObjects, *global);
  }

  return false;
}

bool ArgumentInference::hasUntypedParam(const FunctionDecl *functionDecl) const {
  for (const Param &param : functionDecl->params) {
    if (!param.type.has_value()) {
      return true;
    }
  }

  return false;
}

bool ArgumentInference::isMakingResults(const Expr *arg) const {
  auto *name = dyn_cast<NameExpr>(arg);
  if (name == nullptr) {
    return false;
  }
  auto which = this->state.library.result.resultName(name->name);

  return which == GlobalName::OK || which == GlobalName::ERR;
}

bool ArgumentInference::isBareGenericName(Expr *arg) const {
  auto *name = dyn_cast<NameExpr>(arg);
  if (name == nullptr || !name->typeArgs.empty()) {
    return false;
  }

  return this->state.walk.scopes.findGenericFunction(name) != nullptr;
}

void ArgumentInference::walkArgumentNow(GenericBindingDraft draft, size_t index, const Type *hint) {
  auto &tasks = this->state.walk.tasks;
  Expr **slot = &draft.call->args[index];
  draft.checked[index] = true;
  tasks.pushTask(NextBindingArgument{.draft = std::move(draft)});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = hint});
}

void ArgumentInference::inferFromLiteralText(GenericBindingDraft draft, size_t index,
                                             const StringLitExpr *text) {
  auto &tasks = this->state.walk.tasks;
  const auto *literal = Type::stringLiteral(text->value);
  Expr **slot = &draft.call->args[index];
  draft.checked[index] = true;
  tasks.pushTask(InferFromArgument{
      .draft = std::move(draft),
      .index = index,
      .actual = literal,
      .strength = std::nullopt,
  });

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = literal});
}

void ArgumentInference::inferFromLiteralShape(GenericBindingDraft draft, size_t index,
                                              const Type *shape) {
  draft.literals.push_back(index);

  return this->state.walk.tasks.pushTask(InferFromArgument{
      .draft = std::move(draft),
      .index = index,
      .actual = shape,
      .strength = ArgumentStrength::WEAK,
  });
}

void ArgumentInference::inferFromCheckedArgument(GenericBindingDraft draft, size_t index) {
  auto &tasks = this->state.walk.tasks;
  Expr **slot = &draft.call->args[index];
  draft.checked[index] = true;
  tasks.pushTask(InferFromArgument{
      .draft = std::move(draft),
      .index = index,
      .actual = nullptr,
      .strength = ArgumentStrength::STRONG,
  });

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
}

void ArgumentInference::inferFromArgument(const InferFromArgument &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  this->bindingRules.takeLookups(*draft.inference, draft.pending);
  const Expr *arg = draft.call->args[task.index];
  const Type *actual = task.actual != nullptr ? task.actual : arg->type;
  const auto *pattern = this->bindingRules.parameterFor(draft, task.index);
  bool isIntLiteral = actual->isInteger() && this->state.code.conversions.isIntLiteralTree(arg);
  auto next = *draft.inference;
  this->inferArgument(next, task.strength, isIntLiteral, pattern, actual);
  auto lookups = this->bindingRules.missingLookups(next);
  if (this->bindingRules.hasLookups(lookups)) {
    draft.pending = lookups;
    tasks.pushTask(InferFromArgument{
        .draft = std::move(draft),
        .index = task.index,
        .actual = task.actual,
        .strength = task.strength,
    });

    return this->bindingRules.pushLookups(lookups);
  }
  draft.inference = std::move(next);

  return tasks.pushTask(NextBindingArgument{.draft = std::move(draft)});
}

void ArgumentInference::inferArgument(CandidateInference &inference,
                                      std::optional<ArgumentStrength> strength, bool isIntLiteral,
                                      const Type *pattern, const Type *actual) const {
  if (!strength.has_value()) {
    return inference.from(pattern, actual);
  }
  inference.markArgument(*strength, isIntLiteral);
  inference.from(pattern, actual);

  return inference.forgetArgumentMarks();
}

void ArgumentInference::deferLiteral(GenericBindingDraft draft, size_t index) {
  draft.contextual.push_back(index);
  auto *object = dyn_cast<ObjectLitExpr>(draft.call->args[index]);
  const auto *pattern = this->bindingRules.parameterFor(draft, index);
  if (object == nullptr || !this->isPrecheckingFields(object, pattern)) {
    return this->state.walk.tasks.pushTask(NextBindingArgument{.draft = std::move(draft)});
  }

  return this->state.walk.tasks.pushTask(NextPrecheckedField{
      .draft = std::move(draft),
      .object = object,
      .argument = index,
      .property = 0,
  });
}

bool ArgumentInference::isPrecheckingFields(const ObjectLitExpr *literal,
                                            const Type *pattern) const {
  const Type *target = pattern->nonOptional();
  bool isPlainObject = target->isObject() && !target->isClass() && !target->isTuple();

  return isPlainObject && !literal->hasSpread() && !this->hasSpecialProperty(literal);
}

bool ArgumentInference::hasSpecialProperty(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.method() || property.key() || property.defaulted()) {
      return true;
    }
  }

  return false;
}

void ArgumentInference::nextPrecheckedField(const NextPrecheckedField &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  ObjectLitExpr *object = task.object;
  const Type *target = this->bindingRules.parameterFor(draft, task.argument)->nonOptional();
  size_t property =
      this->nextPrecheckable(object, target, task.property, draft.generic->typeParams);
  if (property == object->props.size()) {
    return tasks.pushTask(NextBindingArgument{.draft = std::move(draft)});
  }
  Expr **slot = &object->props[property].value;
  tasks.pushTask(AfterPrecheckedField{
      .draft = std::move(draft),
      .object = object,
      .argument = task.argument,
      .property = property,
  });

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
}

size_t ArgumentInference::nextPrecheckable(const ObjectLitExpr *object, const Type *target,
                                           size_t from, llvm::ArrayRef<const Type *> params) const {
  for (size_t property = from; property < object->props.size(); ++property) {
    const ObjectLitExpr::Property &entry = object->props[property];
    if (this->isPrecheckable(target->field(entry.name), entry.value, params)) {
      return property;
    }
  }

  return object->props.size();
}

bool ArgumentInference::isPrecheckable(const Field *field, const Expr *value,
                                       llvm::ArrayRef<const Type *> genericTypeParams) const {
  if (field == nullptr || !GenericCheckHelpers::mentionsAnyOf(field->type, genericTypeParams)) {
    return false;
  }
  if (this->isContextSensitive(value) || isa<ObjectLitExpr>(value) || isa<ArrayLitExpr>(value)) {
    return false;
  }

  return this->bindingRules.literalShapeOf(value) == nullptr;
}

void ArgumentInference::afterPrecheckedField(const AfterPrecheckedField &task) {
  this->state.code.literals.markPrechecked(task.object->props[task.property].value);

  return this->inferPrecheckedField(InferPrecheckedField{
      .draft = task.draft,
      .object = task.object,
      .argument = task.argument,
      .property = task.property,
  });
}

void ArgumentInference::inferPrecheckedField(const InferPrecheckedField &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  this->bindingRules.takeLookups(*draft.inference, draft.pending);
  const ObjectLitExpr::Property &entry = task.object->props[task.property];
  const Type *target = this->bindingRules.parameterFor(draft, task.argument)->nonOptional();
  const Type *pattern = target->field(entry.name)->type;
  auto next = *draft.inference;
  next.from(pattern, entry.value->type);
  auto lookups = this->bindingRules.missingLookups(next);
  if (this->bindingRules.hasLookups(lookups)) {
    draft.pending = lookups;
    tasks.pushTask(InferPrecheckedField{
        .draft = std::move(draft),
        .object = task.object,
        .argument = task.argument,
        .property = task.property,
    });

    return this->bindingRules.pushLookups(lookups);
  }
  draft.inference = std::move(next);

  return tasks.pushTask(NextPrecheckedField{
      .draft = std::move(draft),
      .object = task.object,
      .argument = task.argument,
      .property = task.property + 1,
  });
}

void ArgumentInference::inferFromListRest(const InferFromListRest &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  this->bindingRules.takeLookups(*draft.inference, draft.pending);
  const auto *list = draft.listRest != nullptr ? this->listRestArguments(draft) : nullptr;
  if (list == nullptr) {
    return this->settleArguments(std::move(draft));
  }
  auto next = *draft.inference;
  next.from(draft.listRest, list);
  auto lookups = this->bindingRules.missingLookups(next);
  if (this->bindingRules.hasLookups(lookups)) {
    draft.pending = lookups;
    tasks.pushTask(InferFromListRest{.draft = std::move(draft)});

    return this->bindingRules.pushLookups(lookups);
  }
  draft.inference = std::move(next);

  return this->settleArguments(std::move(draft));
}

const Type *ArgumentInference::listRestArguments(const GenericBindingDraft &draft) const {
  const CallExpr *call = draft.call;
  auto parts = std::vector<const Type *>();
  auto spread = std::vector<bool>();
  for (size_t index = draft.fixed; index < call->args.size(); ++index) {
    parts.push_back(call->args[index]->type);
    spread.push_back(call->spreads(index));
  }
  auto why = std::string();

  return ArgumentLists::spreadTuple(parts, spread, why);
}

void ArgumentInference::settleArguments(GenericBindingDraft draft) {
  this->settleWithCandidates(draft);
  if (draft.inference->hasDeferred()) {
    return this->state.walk.tasks.pushTask(RetryDeferred{.draft = std::move(draft)});
  }

  return this->finishInferredArguments(std::move(draft));
}

void ArgumentInference::retryDeferred(const RetryDeferred &task) {
  auto draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  this->bindingRules.takeLookups(*draft.inference, draft.pending);
  auto next = *draft.inference;
  next.retry(draft.bound);
  auto lookups = this->bindingRules.missingLookups(next);
  if (this->bindingRules.hasLookups(lookups)) {
    draft.pending = lookups;
    tasks.pushTask(RetryDeferred{.draft = std::move(draft)});

    return this->bindingRules.pushLookups(lookups);
  }
  draft.inference = std::move(next);
  this->settleWithCandidates(draft);

  return this->finishInferredArguments(std::move(draft));
}

void ArgumentInference::settleWithCandidates(GenericBindingDraft &draft) {
  for (size_t index = 0; index < draft.bound.size(); ++index) {
    if (!draft.inference->getCandidates(index).empty()) {
      this->bindingRules.settleTypeParam(draft, index);
    }
  }
}

void ArgumentInference::finishInferredArguments(GenericBindingDraft draft) {
  this->inferFromExpected(draft);
  for (size_t index = 0; index < draft.bound.size(); ++index) {
    this->bindingRules.settleTypeParam(draft, index);
  }

  return this->state.walk.tasks.pushTask(
      NextContextualArgument{.draft = std::move(draft), .position = 0});
}

void ArgumentInference::inferFromExpected(GenericBindingDraft &draft) {
  const Type *result = draft.result;
  llvm::ArrayRef<const Type *> params = draft.generic->typeParams;
  bool isOpen = llvm::is_contained(draft.bound, nullptr);
  bool isUseful = draft.expected != nullptr && isOpen && result != nullptr &&
                  GenericCheckHelpers::mentionsAnyOf(result, params);
  if (!isUseful) {
    return;
  }
  CandidateInference fromExpected(params, this->state.generics.aliases, IterationLookups::NONE);
  fromExpected.from(result, draft.expected);
  for (size_t index = 0; index < params.size(); ++index) {
    bool hasCandidates = !fromExpected.getCandidates(index).empty();
    if (draft.bound[index] == nullptr && hasCandidates) {
      draft.bound[index] = this->expectedCandidate(fromExpected, index);
    }
  }
}

const Type *ArgumentInference::expectedCandidate(const CandidateInference &fromExpected,
                                                 size_t index) const {
  const Type *type = nullptr;
  for (const Type *candidate : fromExpected.getCandidates(index)) {
    type = type != nullptr ? this->state.types.relations.unify(type, candidate) : candidate;
  }

  return type;
}

}
