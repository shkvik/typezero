#include "compiler/semantic/types/extends_relation/extends_relation.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/template_types/template_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

ExtendsRelation::ExtendsRelation(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classDeclarer = &all.classDeclarer;
  this->templateTypes = &all.templateTypes;
  this->returnInferrer = &all.returnInferrer;
}

bool ExtendsRelation::isExtendingType(const Type *type, const Type *pattern) {
  return this->isExtendingType(type, pattern, nullptr);
}

bool ExtendsRelation::isExtendingType(const Type *type, const Type *pattern,
                                      InferMatch *inference) {
  auto first = ExtendsGoal{.type = type, .pattern = pattern};
  auto steps = std::vector<ExtendsStep>{first};
  while (!steps.empty()) {
    ExtendsStep step = std::move(steps.back());
    steps.pop_back();
    bool isHolding = this->isStepHolding(step, inference, steps);
    if (!isHolding && !this->canRetryChoice(steps, inference)) {
      return false;
    }
  }

  return true;
}

bool ExtendsRelation::isStepHolding(const ExtendsStep &step, InferMatch *inference,
                                    std::vector<ExtendsStep> &steps) {
  switch (step.kind()) {
    case ExtendsStepKind::GOAL:
      return this->isGoalHolding(*step.as<ExtendsGoal>(), inference, steps);
    case ExtendsStepKind::BIND:
      this->bindInfer(*step.as<BindInfer>(), inference);
      return true;
    case ExtendsStepKind::CHOICE:
      return true;
  }
  llvm_unreachable("unknown extends step");
}

bool ExtendsRelation::canRetryChoice(std::vector<ExtendsStep> &steps, InferMatch *inference) const {
  while (!steps.empty()) {
    ExtendsStep step = std::move(steps.back());
    steps.pop_back();
    const ExtendsChoice *choice = step.as<ExtendsChoice>();
    if (choice != nullptr && this->canTryNext(*choice, inference, steps)) {
      return true;
    }
  }

  return false;
}

bool ExtendsRelation::canTryNext(const ExtendsChoice &choice, InferMatch *inference,
                                 std::vector<ExtendsStep> &steps) const {
  if (inference != nullptr) {
    inference->bound = choice.saved;
  }
  auto next = ExtendsChoice{
      .type = choice.type,
      .pattern = choice.pattern,
      .next = choice.next + 1,
      .saved = choice.saved,
  };

  return this->canTryAlternative(std::move(next), steps);
}

bool ExtendsRelation::canTryAlternative(ExtendsChoice choice,
                                        std::vector<ExtendsStep> &steps) const {
  llvm::ArrayRef<const Type *> members = choice.pattern->members();
  if (choice.next >= members.size()) {
    return false;
  }
  auto goal = ExtendsGoal{.type = choice.type, .pattern = members[choice.next]};
  steps.push_back(std::move(choice));
  steps.push_back(goal);

  return true;
}

void ExtendsRelation::bindInfer(const BindInfer &bind, InferMatch *inference) const {
  const Type *&bound = inference->bound[bind.index];
  bool isJoining = bound != nullptr && bound != bind.type;
  bound = isJoining ? Type::unionOf({bound, bind.type}) : bind.type;
}

bool ExtendsRelation::isGoalHolding(const ExtendsGoal &goal, InferMatch *inference,
                                    std::vector<ExtendsStep> &steps) {
  const Type *type = goal.type;
  const Type *pattern = goal.pattern;
  if (inference != nullptr && this->isBindingInferred(type, pattern, inference, steps)) {
    return true;
  }
  if (type == pattern || type->isError() || pattern->isError() || type->isNever()) {
    return true;
  }
  if (pattern->isUnknown()) {
    return true;
  }
  if (pattern->isNever()) {
    return false;
  }
  if (type->isUnion() || type->isOptional()) {
    auto members = TypeopsCheckHelpers::distributedMembers(type);
    this->pushMemberGoals(members, pattern, steps);
    return true;
  }
  if (pattern->isOptional()) {
    this->pushDefinedGoal(type, pattern->inner(), steps);
    return true;
  }
  if (type->isUndefined()) {
    return false;
  }
  if (pattern->isUnion()) {
    return this->canStartChoice(type, pattern, inference, steps);
  }

  return this->isLeafHolding(type, pattern, inference, steps);
}

void ExtendsRelation::pushDefinedGoal(const Type *type, const Type *pattern,
                                      std::vector<ExtendsStep> &steps) const {
  if (!type->isUndefined()) {
    steps.push_back(ExtendsGoal{.type = type, .pattern = pattern});
  }
}

bool ExtendsRelation::canStartChoice(const Type *type, const Type *pattern, InferMatch *inference,
                                     std::vector<ExtendsStep> &steps) const {
  auto saved = std::vector<const Type *>();
  if (inference != nullptr) {
    saved = inference->bound;
  }
  auto choice = ExtendsChoice{
      .type = type,
      .pattern = pattern,
      .next = 0,
      .saved = std::move(saved),
  };

  return this->canTryAlternative(std::move(choice), steps);
}

bool ExtendsRelation::isBindingInferred(const Type *type, const Type *pattern,
                                        InferMatch *inference,
                                        std::vector<ExtendsStep> &steps) const {
  auto found = llvm::find(inference->params, pattern);
  if (found == inference->params.end()) {
    return false;
  }
  size_t index = size_t(found - inference->params.begin());
  steps.push_back(BindInfer{.index = index, .type = type});
  const Type *constraint = pattern->constraint();
  if (constraint != nullptr) {
    steps.push_back(ExtendsGoal{.type = type, .pattern = constraint});
  }

  return true;
}

bool ExtendsRelation::isLeafHolding(const Type *type, const Type *pattern, InferMatch *inference,
                                    std::vector<ExtendsStep> &steps) {
  if (pattern->typeOp() == TypeOp::Template) {
    return type->isStringLiteral() && this->templateTypes->isMatchingTemplate(
                                          type->literalText(), pattern->operands(), inference);
  }
  if (pattern->isPattern()) {
    return type->isStringLiteral() &&
           TemplatePatterns::matchesPattern(type->literalText(), pattern);
  }
  if (type->isPattern()) {
    return pattern == Type::string();
  }
  if (type->isLiteral() && pattern->isLiteral()) {
    return false;
  }
  if (type->isLiteral()) {
    steps.push_back(ExtendsGoal{.type = type->literalBase(), .pattern = pattern});
    return true;
  }
  if (pattern->isLiteral() || type->isParam() || pattern->isParam()) {
    return false;
  }
  if (pattern->isAnyObject()) {
    return UnknownChecker::isObjectReference(type);
  }

  return this->isStructureHolding(type, pattern, inference, steps);
}

bool ExtendsRelation::isStructureHolding(const Type *type, const Type *pattern,
                                         InferMatch *inference, std::vector<ExtendsStep> &steps) {
  if (type->isArray()) {
    return this->isArrayHolding(type, pattern, steps);
  }
  if (type->isTuple()) {
    return this->isTupleHolding(type, pattern, inference, steps);
  }
  if (type->isFunction()) {
    return pattern->isFunction() && this->isExtendingFunction(type, pattern, inference);
  }
  if (type->isPromise()) {
    return this->isPromiseHolding(type, pattern, steps);
  }
  if (type->isIterator()) {
    return this->isIteratorHolding(type, pattern, steps);
  }
  if (type->isDict()) {
    return this->isDictHolding(type, pattern, steps);
  }
  if (type->isResult()) {
    return this->isResultHolding(type, pattern, steps);
  }
  if (type->isObject() && pattern->isObject()) {
    return this->isExtendingObject(type, pattern, inference);
  }

  return !pattern->isObject() && !type->isObject() &&
         TypeRelations::isImplicitlyConvertible(type, pattern);
}

bool ExtendsRelation::isArrayHolding(const Type *type, const Type *pattern,
                                     std::vector<ExtendsStep> &steps) const {
  if (!pattern->isArray()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->inner(), .pattern = pattern->inner()});

  return true;
}

bool ExtendsRelation::isTupleHolding(const Type *type, const Type *pattern, InferMatch *inference,
                                     std::vector<ExtendsStep> &steps) {
  if (!pattern->isArray()) {
    return pattern->isTuple() && this->isExtendingTuple(type, pattern, inference);
  }
  auto elements = this->fieldTypes(type);
  if (type->tupleRest() != nullptr) {
    elements.push_back(type->tupleRest()->inner());
  }
  this->pushMemberGoals(elements, pattern->inner(), steps);

  return true;
}

bool ExtendsRelation::isPromiseHolding(const Type *type, const Type *pattern,
                                       std::vector<ExtendsStep> &steps) const {
  if (!pattern->isPromise()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->promised(), .pattern = pattern->promised()});

  return true;
}

bool ExtendsRelation::isIteratorHolding(const Type *type, const Type *pattern,
                                        std::vector<ExtendsStep> &steps) const {
  if (!pattern->isIterator()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->sent(), .pattern = pattern->sent()});
  steps.push_back(ExtendsGoal{.type = type->returned(), .pattern = pattern->returned()});
  steps.push_back(ExtendsGoal{.type = type->yielded(), .pattern = pattern->yielded()});

  return true;
}

bool ExtendsRelation::isDictHolding(const Type *type, const Type *pattern,
                                    std::vector<ExtendsStep> &steps) const {
  if (!pattern->isDict() || type->dictKind() != pattern->dictKind()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->valueType(), .pattern = pattern->valueType()});
  steps.push_back(ExtendsGoal{.type = type->keyType(), .pattern = pattern->keyType()});

  return true;
}

bool ExtendsRelation::isResultHolding(const Type *type, const Type *pattern,
                                      std::vector<ExtendsStep> &steps) const {
  if (!pattern->isResult()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->errType(), .pattern = pattern->errType()});
  steps.push_back(ExtendsGoal{.type = type->okType(), .pattern = pattern->okType()});

  return true;
}

void ExtendsRelation::pushMemberGoals(llvm::ArrayRef<const Type *> types, const Type *pattern,
                                      std::vector<ExtendsStep> &steps) const {
  for (const Type *type : llvm::reverse(types)) {
    steps.push_back(ExtendsGoal{.type = type, .pattern = pattern});
  }
}

std::vector<const Type *> ExtendsRelation::fieldTypes(const Type *object) const {
  auto types = std::vector<const Type *>();
  for (const Field &field : object->fields()) {
    types.push_back(field.type);
  }

  return types;
}

bool ExtendsRelation::isExtendingEach(llvm::ArrayRef<const Type *> types,
                                      llvm::ArrayRef<const Type *> patterns,
                                      InferMatch *inference) {
  for (size_t index = 0; index < types.size(); ++index) {
    if (!this->isExtendingType(types[index], patterns[index], inference)) {
      return false;
    }
  }

  return true;
}

bool ExtendsRelation::isExtendingTuple(const Type *type, const Type *pattern,
                                       InferMatch *inference) {
  auto mine = this->fieldTypes(type);
  auto theirs = this->fieldTypes(pattern);
  auto spread = this->soleSpread(theirs);
  if (!spread.has_value()) {
    return false;
  }
  bool isOpen = type->tupleRest() != nullptr || type->tupleMin() < mine.size();
  if (isOpen) {
    bool isOnlySpread = *spread == 0 && theirs.size() == 1;

    return isOnlySpread && this->isExtendingType(type, theirs[0], inference);
  }
  if (*spread == theirs.size()) {
    return this->isExtendingPlainTuple(mine, pattern, inference);
  }

  return this->isExtendingSpreadTuple(mine, theirs, *spread, inference);
}

std::optional<size_t> ExtendsRelation::soleSpread(llvm::ArrayRef<const Type *> elements) const {
  size_t spread = elements.size();
  for (size_t index = 0; index < elements.size(); ++index) {
    bool isSpread = elements[index]->isListParam();
    if (isSpread && spread != elements.size()) {
      return std::nullopt;
    }
    if (isSpread) {
      spread = index;
    }
  }

  return spread;
}

bool ExtendsRelation::isExtendingPlainTuple(llvm::ArrayRef<const Type *> mine, const Type *pattern,
                                            InferMatch *inference) {
  auto theirs = this->fieldTypes(pattern);
  if (pattern->tupleRest() != nullptr) {
    return this->isExtendingRestTuple(mine, theirs, pattern->tupleRest()->inner(), inference);
  }
  if (mine.size() != theirs.size() || pattern->tupleMin() < theirs.size()) {
    return false;
  }

  return this->isExtendingEach(mine, theirs, inference);
}

bool ExtendsRelation::isExtendingRestTuple(llvm::ArrayRef<const Type *> mine,
                                           llvm::ArrayRef<const Type *> theirs,
                                           const Type *restElement, InferMatch *inference) {
  if (mine.size() < theirs.size()) {
    return false;
  }
  for (size_t index = 0; index < mine.size(); ++index) {
    const Type *wanted = index < theirs.size() ? theirs[index] : restElement;
    if (!this->isExtendingType(mine[index], wanted, inference)) {
      return false;
    }
  }

  return true;
}

bool ExtendsRelation::isExtendingSpreadTuple(llvm::ArrayRef<const Type *> mine,
                                             llvm::ArrayRef<const Type *> theirs, size_t spread,
                                             InferMatch *inference) {
  size_t after = theirs.size() - spread - 1;
  if (mine.size() < spread + after) {
    return false;
  }
  auto middle = mine.slice(spread, mine.size() - spread - after);
  bool isHeadFitting = this->isExtendingEach(mine.take_front(spread), theirs, inference);
  if (!isHeadFitting) {
    return false;
  }
  bool isTailFitting =
      this->isExtendingEach(mine.take_back(after), theirs.take_back(after), inference);
  if (!isTailFitting) {
    return false;
  }
  const auto *gathered = Type::tuple(middle);

  return this->isExtendingType(gathered, theirs[spread], inference);
}

bool ExtendsRelation::isExtendingFunction(const Type *type, const Type *pattern,
                                          InferMatch *inference) {
  if (!pattern->ret()->isVoid() && !this->isExtendingType(type->ret(), pattern->ret(), inference)) {
    return false;
  }
  if (!pattern->hasRest() && type->minArgs() > pattern->params().size()) {
    return false;
  }
  size_t theirFixed = this->fixedParamCount(pattern);
  for (size_t index = 0; index < theirFixed; ++index) {
    const Type *my = this->paramAt(type, index);
    if (my != nullptr && !this->isParamFitting(my, pattern->params()[index], inference)) {
      return false;
    }
  }
  if (!pattern->hasRest()) {
    return true;
  }

  return this->isExtendingRest(type, pattern, inference);
}

size_t ExtendsRelation::fixedParamCount(const Type *function) const {
  size_t restCount = function->hasRest() ? 1 : 0;

  return function->params().size() - restCount;
}

const Type *ExtendsRelation::paramAt(const Type *function, size_t index) const {
  llvm::ArrayRef<const Type *> params = function->params();
  if (index < this->fixedParamCount(function)) {
    return params[index];
  }
  if (function->hasRest() && params.back()->isArray()) {
    return params.back()->inner();
  }

  return nullptr;
}

bool ExtendsRelation::isParamFitting(const Type *my, const Type *their, InferMatch *inference) {
  if (inference != nullptr && GenericCheckHelpers::mentionsAnyOf(their, inference->params)) {
    return this->isExtendingType(my, their, inference);
  }

  return TypeRelations::isImplicitlyConvertible(their, my) || this->isExtendingType(their, my);
}

bool ExtendsRelation::isExtendingRest(const Type *type, const Type *pattern,
                                      InferMatch *inference) {
  const Type *rest = pattern->params().back();
  size_t myFixed = this->fixedParamCount(type);
  size_t theirFixed = this->fixedParamCount(pattern);
  if (inference != nullptr && llvm::is_contained(inference->params, rest)) {
    const Type *arguments = this->restArguments(type, std::min(theirFixed, myFixed));

    return this->isExtendingType(arguments, rest, inference);
  }
  const Type *element = rest->isArray() ? rest->inner() : nullptr;
  if (element == nullptr || element->isUnknown() || element->isNever()) {
    return true;
  }
  for (size_t index = theirFixed; index < myFixed; ++index) {
    if (!this->isParamFitting(type->params()[index], element, inference)) {
      return false;
    }
  }

  return true;
}

const Type *ExtendsRelation::restArguments(const Type *function, size_t from) const {
  llvm::ArrayRef<const Type *> params = function->params();
  size_t fixed = this->fixedParamCount(function);
  auto elements =
      std::vector<const Type *>(params.begin() + long(from), params.begin() + long(fixed));
  size_t required = function->minArgs() > from ? function->minArgs() - from : 0;
  const Type *rest = function->hasRest() ? params.back() : nullptr;

  return Type::argumentList(elements, std::min(required, elements.size()), rest);
}

bool ExtendsRelation::isExtendingObject(const Type *type, const Type *pattern,
                                        InferMatch *inference) {
  if (type->isSubtypeOf(pattern)) {
    return true;
  }
  if (pattern->isClass()) {
    return this->isExtendingClass(type, pattern, inference);
  }
  bool isStructural = !type->isTuple() && !pattern->isTuple() && !type->isAnyObject() &&
                      pattern->overloads().empty();
  if (!isStructural) {
    return false;
  }
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
  }
  for (const Field &field : pattern->fields()) {
    if (!this->isExtendingField(type, classDecl, field, inference)) {
      return false;
    }
  }

  return true;
}

bool ExtendsRelation::isExtendingClass(const Type *type, const Type *pattern,
                                       InferMatch *inference) {
  if (pattern->typeArgs().empty()) {
    return false;
  }
  const ClassDecl *wanted = this->genericOrigin(pattern->classDecl());
  auto pending = std::vector<const Type *>{type};
  while (!pending.empty()) {
    const Type *candidate = pending.back();
    pending.pop_back();
    bool isSameGeneric = this->isInstanceOf(candidate, wanted, pattern->typeArgs().size());
    if (isSameGeneric &&
        this->isExtendingEach(candidate->typeArgs(), pattern->typeArgs(), inference)) {
      return true;
    }
    pending.insert(pending.end(), candidate->bases().begin(), candidate->bases().end());
  }

  return false;
}

const ClassDecl *ExtendsRelation::genericOrigin(const ClassDecl *classDecl) const {
  if (classDecl->generic() != nullptr) {
    return classDecl->generic();
  }

  return classDecl;
}

bool ExtendsRelation::isInstanceOf(const Type *candidate, const ClassDecl *generic,
                                   size_t typeArgCount) const {
  const ClassDecl *classDecl = candidate->classDecl();
  if (classDecl == nullptr || this->genericOrigin(classDecl) != generic) {
    return false;
  }

  return candidate->typeArgs().size() == typeArgCount;
}

bool ExtendsRelation::isExtendingField(const Type *type, const ClassDecl *classDecl,
                                       const Field &field, InferMatch *inference) {
  const Type *own = this->memberType(type, classDecl, field.name);
  if (own == nullptr) {
    return field.type->isOptional();
  }

  return this->isExtendingType(own, field.type, inference);
}

const Type *ExtendsRelation::memberType(const Type *type, const ClassDecl *classDecl,
                                        llvm::StringRef name) const {
  const Field *have = type->field(name);
  if (have != nullptr) {
    return have->type;
  }
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  bool isPlainMethod =
      !methods.empty() && methods.back()->sym != nullptr && methods.back()->typeParams.empty();
  if (!isPlainMethod) {
    return nullptr;
  }

  return this->returnInferrer->functionType(methods.back()->sym);
}

}
