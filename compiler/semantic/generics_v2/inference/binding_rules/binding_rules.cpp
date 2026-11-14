#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

BindingRules::BindingRules(SemanticState &owner)
    : state(owner) {}

const Type *BindingRules::literalShapeOf(const Expr *expr) const {
  auto answers = std::vector<const Type *>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &BindingRules::shapeParts)) {
    const auto *shape = this->nodeShape(node, answers);
    if (shape == nullptr) {
      return nullptr;
    }
    answers.push_back(shape);
  }

  return answers.back();
}

NodeChildren<const Expr *> BindingRules::shapeParts(const Expr *expr) const {
  if (SharedHelpers::literalType(expr) != nullptr) {
    return {};
  }
  auto *array = dyn_cast<ArrayLitExpr>(expr);
  auto *object = dyn_cast<ObjectLitExpr>(expr);
  auto parts = NodeChildren<const Expr *>();
  if (array != nullptr && !this->hasGaps(array)) {
    for (const ArrayLitExpr::Element &element : array->elements) {
      parts.push_back(element.expr);
    }
  }
  if (object != nullptr) {
    for (const ObjectLitExpr::Property &property : object->props) {
      parts.push_back(property.value);
    }
  }

  return parts;
}

bool BindingRules::hasGaps(const ArrayLitExpr *literal) const {
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread || element.hole) {
      return true;
    }
  }

  return false;
}

const Type *BindingRules::nodeShape(const Expr *expr, std::vector<const Type *> &answers) const {
  const auto *type = SharedHelpers::literalType(expr);
  if (type != nullptr) {
    return isa<TemplateExpr>(expr) ? nullptr : type;
  }
  auto *array = dyn_cast<ArrayLitExpr>(expr);
  auto *object = dyn_cast<ObjectLitExpr>(expr);
  if (isa<UndefinedExpr>(expr)) {
    return Type::undefinedTy();
  }
  if (array != nullptr) {
    return this->arrayShape(array, answers);
  }
  if (object != nullptr) {
    return this->objectShape(object, answers);
  }

  return nullptr;
}

const Type *BindingRules::arrayShape(const ArrayLitExpr *literal,
                                     std::vector<const Type *> &answers) const {
  if (this->hasGaps(literal)) {
    return nullptr;
  }
  std::vector<const Type *> types = ChildrenFirst::takeAnswers(answers, literal->elements.size());
  const Type *element = nullptr;
  for (const Type *type : types) {
    element = element != nullptr ? this->state.types.relations.unify(element, type) : type;
    if (element == nullptr) {
      return nullptr;
    }
  }

  return element != nullptr && !element->isUndefined() ? Type::array(element) : nullptr;
}

const Type *BindingRules::objectShape(const ObjectLitExpr *literal,
                                      std::vector<const Type *> &answers) const {
  std::vector<const Type *> types = ChildrenFirst::takeAnswers(answers, literal->props.size());
  auto fields = std::vector<Field>();
  for (size_t index = 0; index < types.size(); ++index) {
    const ObjectLitExpr::Property &property = literal->props[index];
    bool isRepeated = this->hasFieldNamed(fields, property.name);
    if (types[index]->isUndefined() || isRepeated) {
      return nullptr;
    }
    auto field = Field{.name = property.name.str(), .type = types[index], .readonly = false};
    fields.push_back(field);
  }

  return Type::object(std::move(fields));
}

bool BindingRules::hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return true;
    }
  }

  return false;
}

const Type *BindingRules::parameterFor(const GenericBindingDraft &draft, size_t index) const {
  const FuncSymbol *generic = draft.generic;
  if (!generic->hasRest || index < draft.fixed) {
    return generic->params[index];
  }
  const Type *rest = generic->params.back();
  bool isElement = !draft.call->spreads(index) && rest->isArray();

  return isElement ? rest->inner() : rest;
}

void BindingRules::takeLookups(CandidateInference &inference, PendingLookups &pending) {
  auto &tasks = this->state.walk.tasks;
  auto asyncTypes = tasks.takeAnswers(pending.asyncTypes.size());
  auto elementTypes = tasks.takeAnswers(pending.elementTypes.size());
  for (size_t index = 0; index < asyncTypes.size(); ++index) {
    inference.rememberAsyncType(pending.asyncTypes[index], asyncTypes[index]);
  }
  for (size_t index = 0; index < elementTypes.size(); ++index) {
    inference.rememberElementType(pending.elementTypes[index], elementTypes[index]);
  }
  pending = PendingLookups{.elementTypes = {}, .asyncTypes = {}};
}

PendingLookups BindingRules::missingLookups(const CandidateInference &inference) const {
  return PendingLookups{
      .elementTypes = inference.getMissingElementTypes(),
      .asyncTypes = inference.getMissingAsyncTypes(),
  };
}

bool BindingRules::hasLookups(const PendingLookups &pending) const {
  return !pending.elementTypes.empty() || !pending.asyncTypes.empty();
}

void BindingRules::pushLookups(const PendingLookups &pending) {
  auto &tasks = this->state.walk.tasks;
  for (const Type *type : llvm::reverse(pending.asyncTypes)) {
    tasks.pushTask(AsyncIterableTypeOf{.type = type});
  }
  for (const Type *type : llvm::reverse(pending.elementTypes)) {
    tasks.pushTask(ElementTypeOf{.type = type});
  }
}

void BindingRules::settleTypeParam(GenericBindingDraft &draft, size_t index) {
  if (draft.bound[index] != nullptr) {
    return;
  }
  auto joined = this->joinCandidates(draft, index);
  if (joined.isErr()) {
    this->state.diagnostics.error(draft.loc, joined.error().message);
    draft.bound[index] = Type::error();
    draft.isInferred = false;
    return;
  }
  const auto *widened = this->widenToConstraint(draft, index, joined.value());
  draft.bound[index] = this->withUndefined(*draft.inference, index, widened);
}

BindingRules::ResultCandidateType BindingRules::joinCandidates(const GenericBindingDraft &draft,
                                                               size_t index) const {
  auto strong = this->joinStrongCandidates(draft, index);
  if (strong.isErr()) {
    return err(strong.error());
  }
  auto weak = this->joinWeakCandidates(draft, index, strong.value());
  if (weak.isErr()) {
    return err(weak.error());
  }

  return this->joinLiteralCandidates(draft, index, weak.value());
}

BindingRules::ResultCandidateType
BindingRules::joinStrongCandidates(const GenericBindingDraft &draft, size_t index) const {
  const Type *param = draft.generic->typeParams[index];
  const Type *type = nullptr;
  for (const Type *candidate : draft.inference->getCandidates(index)) {
    auto joined = this->candidatesJoin(draft.generic, param, type, candidate);
    if (joined.isErr()) {
      return err(GenericsInferenceRegistryJoinCandidatesError{
          .code = GenericsInferenceRegistryJoinCandidatesErrorCode::CANDIDATES_CLASH,
          .message = joined.error().message,
      });
    }
    type = joined.value();
  }

  return ok(type);
}

BindingRules::ResultCandidateType BindingRules::joinWeakCandidates(const GenericBindingDraft &draft,
                                                                   size_t index,
                                                                   const Type *strong) const {
  const Type *param = draft.generic->typeParams[index];
  bool isStrong = !draft.inference->getCandidates(index).empty();
  const Type *type = strong;
  for (const Type *candidate : draft.inference->getWeakCandidates(index)) {
    auto joined = this->candidatesJoin(draft.generic, param, type, candidate);
    if (joined.isErr() && !isStrong) {
      return err(GenericsInferenceRegistryJoinCandidatesError{
          .code = GenericsInferenceRegistryJoinCandidatesErrorCode::CANDIDATES_CLASH,
          .message = joined.error().message,
      });
    }
    if (joined.isOk()) {
      type = joined.value();
    }
  }

  return ok(type);
}

BindingRules::ResultCandidateType
BindingRules::joinLiteralCandidates(const GenericBindingDraft &draft, size_t index,
                                    const Type *known) const {
  const Type *param = draft.generic->typeParams[index];
  const Type *type = known;
  for (const Type *candidate : draft.inference->getLiteralCandidates(index)) {
    if (!this->isOtherNumber(type, candidate)) {
      auto joined = this->candidatesJoin(draft.generic, param, type, candidate);
      if (joined.isErr()) {
        return err(GenericsInferenceRegistryJoinCandidatesError{
            .code = GenericsInferenceRegistryJoinCandidatesErrorCode::CANDIDATES_CLASH,
            .message = joined.error().message,
        });
      }
      type = joined.value();
    }
  }

  return ok(type);
}

BindingRules::ResultCandidateJoin BindingRules::candidatesJoin(const FuncSymbol *generic,
                                                               const Type *param, const Type *type,
                                                               const Type *candidate) const {
  const Type *next =
      type != nullptr ? this->state.types.relations.unify(type, candidate) : candidate;
  if (next == nullptr) {
    auto message = ErrorTextCandidates::conflict(param->name(), generic->name, type->name(),
                                                 candidate->name());

    return err(GenericsInferenceRegistryCandidatesJoinError{
        .code = GenericsInferenceRegistryCandidatesJoinErrorCode::CONFLICTING_CANDIDATES,
        .message = message,
    });
  }

  return ok(next);
}

bool BindingRules::isOtherNumber(const Type *type, const Type *candidate) const {
  if (type == nullptr || type == candidate) {
    return false;
  }

  return type->isNumeric() || type->isDecimal();
}

const Type *BindingRules::widenToConstraint(const GenericBindingDraft &draft, size_t index,
                                            const Type *type) const {
  const Type *constraint = draft.generic->typeParams[index]->constraint();
  if (constraint != nullptr && constraint->isUnion()) {
    constraint = this->state.types.literals.numberMemberOf(constraint, NumberSpelling::WHOLE);
  }
  const CandidateInference &inference = *draft.inference;
  bool isOnlyLiterals =
      inference.getCandidates(index).empty() && inference.getWeakCandidates(index).empty();
  bool isFractional = constraint != nullptr && (constraint->isFloat() || constraint->isDecimal());
  bool isWidening = type != nullptr && type->isInteger() && isOnlyLiterals && isFractional;

  return isWidening ? constraint : type;
}

const Type *BindingRules::withUndefined(const CandidateInference &inference, size_t index,
                                        const Type *type) const {
  if (!inference.hasSeenUndefined(index)) {
    return type;
  }

  return type != nullptr ? Type::optional(type) : Type::voidTy();
}

void BindingRules::finishBinding(GenericBindingDraft draft) {
  auto &tasks = this->state.walk.tasks;
  auto missing =
      this->hasArgumentErrors(draft) ? MissingTypeArgs::SILENT : MissingTypeArgs::REPORTED;
  auto finish = FinishTypeArgs{
      .generic = draft.generic,
      .bound = draft.bound,
      .loc = draft.loc,
      .use = TypeArgUse::CALL,
      .missing = missing,
  };
  tasks.pushTask(AfterBindingTypeArgs{.draft = std::move(draft)});

  return tasks.pushTask(std::move(finish));
}

bool BindingRules::hasArgumentErrors(const GenericBindingDraft &draft) const {
  bool isReported = this->state.diagnostics.getWrittenErrorCount() > draft.errorsBefore;
  if (!isReported) {
    return false;
  }
  for (size_t index = 0; index < draft.call->args.size(); ++index) {
    if (draft.checked[index] && draft.call->args[index]->type->isError()) {
      return true;
    }
  }

  return false;
}

void BindingRules::deliverFailure(const GenericBindingDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  switch (draft.owner) {
    case BindingOwner::BIND:
      return tasks.pushVerdict(false);
    case BindingOwner::CALL:
      return tasks.pushChecked(draft.call);
  }
}

void BindingRules::pushBound(llvm::ArrayRef<const Type *> bound) {
  auto &tasks = this->state.walk.tasks;
  for (const Type *type : bound) {
    tasks.pushAnswer(type);
  }

  return tasks.pushVerdict(true);
}

}
