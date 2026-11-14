#include "compiler/semantic/types_v2/relations/types_relations.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include "compiler/program/library/number/number_method_table/number_method_table.h"
#include "compiler/program/library/regex/match_member_table/match_member_table.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/semantic_state/v2/types/type_storage.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cctype>
#include <cmath>

namespace tz {

namespace {

constexpr unsigned kMaxFitsDepth = 8;
constexpr size_t kFitsWayCount = 3;
constexpr double kMaxWholeNumber = 9007199254740992.0;
constexpr double kMaxExactInteger = 9007199254740992.0;

}

TypesRelations::TypesRelations(TypeStorage &owner)
    : types(owner) {}

llvm::ArrayRef<const Type *> TypesRelations::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool TypesRelations::isExpectingLiteral(const Type *param) const {
  if (!param->isParam()) {
    return false;
  }
  const Type *constraint = param->constraint();
  if (constraint == nullptr) {
    return false;
  }
  if (constraint->typeOp() == TypeOp::KeyOf) {
    return true;
  }
  for (const Type *member : this->membersOf(constraint)) {
    if (!member->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

bool TypesRelations::isExtendingType(const Type *type, const Type *pattern) {
  return this->isExtendingType(type, pattern, nullptr);
}

bool TypesRelations::isExtendingType(const Type *type, const Type *pattern, InferMatch *inference) {
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

bool TypesRelations::isStepHolding(const ExtendsStep &step, InferMatch *inference,
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

bool TypesRelations::canRetryChoice(std::vector<ExtendsStep> &steps, InferMatch *inference) const {
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

bool TypesRelations::canTryNext(const ExtendsChoice &choice, InferMatch *inference,
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

bool TypesRelations::canTryAlternative(ExtendsChoice choice,
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

void TypesRelations::bindInfer(const BindInfer &bind, InferMatch *inference) const {
  const Type *&bound = inference->bound[bind.index];
  bool isJoining = bound != nullptr && bound != bind.type;
  bound = isJoining ? Type::unionOf({bound, bind.type}) : bind.type;
}

bool TypesRelations::isGoalHolding(const ExtendsGoal &goal, InferMatch *inference,
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

void TypesRelations::pushDefinedGoal(const Type *type, const Type *pattern,
                                     std::vector<ExtendsStep> &steps) const {
  if (!type->isUndefined()) {
    steps.push_back(ExtendsGoal{.type = type, .pattern = pattern});
  }
}

bool TypesRelations::canStartChoice(const Type *type, const Type *pattern, InferMatch *inference,
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

bool TypesRelations::isBindingInferred(const Type *type, const Type *pattern, InferMatch *inference,
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

bool TypesRelations::isLeafHolding(const Type *type, const Type *pattern, InferMatch *inference,
                                   std::vector<ExtendsStep> &steps) {
  if (pattern->typeOp() == TypeOp::Template) {
    return type->isStringLiteral() &&
           this->isMatchingTemplate(type->literalText(), pattern->operands(), inference);
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

bool TypesRelations::isStructureHolding(const Type *type, const Type *pattern,
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

  return !pattern->isObject() && !type->isObject() && this->isImplicitlyConvertible(type, pattern);
}

bool TypesRelations::isArrayHolding(const Type *type, const Type *pattern,
                                    std::vector<ExtendsStep> &steps) const {
  if (!pattern->isArray()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->inner(), .pattern = pattern->inner()});

  return true;
}

bool TypesRelations::isTupleHolding(const Type *type, const Type *pattern, InferMatch *inference,
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

bool TypesRelations::isPromiseHolding(const Type *type, const Type *pattern,
                                      std::vector<ExtendsStep> &steps) const {
  if (!pattern->isPromise()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->promised(), .pattern = pattern->promised()});

  return true;
}

bool TypesRelations::isIteratorHolding(const Type *type, const Type *pattern,
                                       std::vector<ExtendsStep> &steps) const {
  if (!pattern->isIterator()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->sent(), .pattern = pattern->sent()});
  steps.push_back(ExtendsGoal{.type = type->returned(), .pattern = pattern->returned()});
  steps.push_back(ExtendsGoal{.type = type->yielded(), .pattern = pattern->yielded()});

  return true;
}

bool TypesRelations::isDictHolding(const Type *type, const Type *pattern,
                                   std::vector<ExtendsStep> &steps) const {
  if (!pattern->isDict() || type->dictKind() != pattern->dictKind()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->valueType(), .pattern = pattern->valueType()});
  steps.push_back(ExtendsGoal{.type = type->keyType(), .pattern = pattern->keyType()});

  return true;
}

bool TypesRelations::isResultHolding(const Type *type, const Type *pattern,
                                     std::vector<ExtendsStep> &steps) const {
  if (!pattern->isResult()) {
    return false;
  }
  steps.push_back(ExtendsGoal{.type = type->errType(), .pattern = pattern->errType()});
  steps.push_back(ExtendsGoal{.type = type->okType(), .pattern = pattern->okType()});

  return true;
}

void TypesRelations::pushMemberGoals(llvm::ArrayRef<const Type *> types, const Type *pattern,
                                     std::vector<ExtendsStep> &steps) const {
  for (const Type *type : llvm::reverse(types)) {
    steps.push_back(ExtendsGoal{.type = type, .pattern = pattern});
  }
}

std::vector<const Type *> TypesRelations::fieldTypes(const Type *object) const {
  auto types = std::vector<const Type *>();
  for (const Field &field : object->fields()) {
    types.push_back(field.type);
  }

  return types;
}

bool TypesRelations::isExtendingEach(llvm::ArrayRef<const Type *> types,
                                     llvm::ArrayRef<const Type *> patterns, InferMatch *inference) {
  for (size_t index = 0; index < types.size(); ++index) {
    if (!this->isExtendingType(types[index], patterns[index], inference)) {
      return false;
    }
  }

  return true;
}

bool TypesRelations::isExtendingTuple(const Type *type, const Type *pattern,
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

std::optional<size_t> TypesRelations::soleSpread(llvm::ArrayRef<const Type *> elements) const {
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

bool TypesRelations::isExtendingPlainTuple(llvm::ArrayRef<const Type *> mine, const Type *pattern,
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

bool TypesRelations::isExtendingRestTuple(llvm::ArrayRef<const Type *> mine,
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

bool TypesRelations::isExtendingSpreadTuple(llvm::ArrayRef<const Type *> mine,
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

bool TypesRelations::isExtendingFunction(const Type *type, const Type *pattern,
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

size_t TypesRelations::fixedParamCount(const Type *function) const {
  size_t restCount = function->hasRest() ? 1 : 0;

  return function->params().size() - restCount;
}

const Type *TypesRelations::paramAt(const Type *function, size_t index) const {
  llvm::ArrayRef<const Type *> params = function->params();
  if (index < this->fixedParamCount(function)) {
    return params[index];
  }
  if (function->hasRest() && params.back()->isArray()) {
    return params.back()->inner();
  }

  return nullptr;
}

bool TypesRelations::isParamFitting(const Type *my, const Type *their, InferMatch *inference) {
  if (inference != nullptr && GenericCheckHelpers::mentionsAnyOf(their, inference->params)) {
    return this->isExtendingType(my, their, inference);
  }

  return this->isImplicitlyConvertible(their, my) || this->isExtendingType(their, my);
}

bool TypesRelations::isExtendingRest(const Type *type, const Type *pattern, InferMatch *inference) {
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

const Type *TypesRelations::restArguments(const Type *function, size_t from) const {
  llvm::ArrayRef<const Type *> params = function->params();
  size_t fixed = this->fixedParamCount(function);
  auto elements =
      std::vector<const Type *>(params.begin() + long(from), params.begin() + long(fixed));
  size_t required = function->minArgs() > from ? function->minArgs() - from : 0;
  const Type *rest = function->hasRest() ? params.back() : nullptr;

  return Type::argumentList(elements, std::min(required, elements.size()), rest);
}

bool TypesRelations::isExtendingObject(const Type *type, const Type *pattern,
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
  const ClassDecl *classDecl = type->classDecl();
  for (const Field &field : pattern->fields()) {
    if (!this->isExtendingField(type, classDecl, field, inference)) {
      return false;
    }
  }

  return true;
}

bool TypesRelations::isExtendingClass(const Type *type, const Type *pattern,
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

const ClassDecl *TypesRelations::genericOrigin(const ClassDecl *classDecl) const {
  if (classDecl->generic() != nullptr) {
    return classDecl->generic();
  }

  return classDecl;
}

bool TypesRelations::isInstanceOf(const Type *candidate, const ClassDecl *generic,
                                  size_t typeArgCount) const {
  const ClassDecl *classDecl = candidate->classDecl();
  if (classDecl == nullptr || this->genericOrigin(classDecl) != generic) {
    return false;
  }

  return candidate->typeArgs().size() == typeArgCount;
}

bool TypesRelations::isExtendingField(const Type *type, const ClassDecl *classDecl,
                                      const Field &field, InferMatch *inference) {
  auto *unknown = this->unknownMethod(type, classDecl, field.name);
  if (unknown != nullptr && this->neededFunctions != nullptr) {
    this->neededFunctions->push_back(unknown);
    return true;
  }
  const Type *own = this->memberType(type, classDecl, field.name);
  if (own == nullptr) {
    return field.type->isOptional();
  }

  return this->isExtendingType(own, field.type, inference);
}

const Type *TypesRelations::memberType(const Type *type, const ClassDecl *classDecl,
                                       llvm::StringRef name) const {
  const Field *have = type->field(name);
  if (have != nullptr) {
    return have->type;
  }
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto methods = classDecl->findMethodsNamed(name);
  bool isPlainMethod =
      !methods.empty() && methods.back()->sym != nullptr && methods.back()->typeParams.empty();
  if (!isPlainMethod) {
    return nullptr;
  }

  const FuncSymbol *function = methods.back()->sym;

  return this->knownFunctionType(function);
}

bool TypesRelations::isHoldingWideInteger(const Type *type) {
  auto parts = TypeParts::collect(type, kWideIntegerRoute);
  for (const Type *part : parts.getTypes()) {
    if (this->isWideInteger(part)) {
      return true;
    }
  }

  return false;
}

bool TypesRelations::isWideInteger(const Type *type) {
  if (!type->isInteger()) {
    return false;
  }

  return double(type->intMax()) > kMaxExactInteger || double(type->intMin()) < -kMaxExactInteger;
}

bool TypesRelations::isCopyingIntoRecord(const Type *from, const Type *to) {
  if (!this->hasRecordCopyShape(from, to)) {
    return false;
  }
  const Type *value = to->valueType();
  for (const Field &field : from->fields()) {
    const auto *type = this->copiedFieldType(field, value);
    if (type->isFunction() || !this->isImplicitlyConvertible(type, value)) {
      return false;
    }
  }

  return true;
}

bool TypesRelations::hasRecordCopyShape(const Type *from, const Type *to) {
  return to->isRecord() && to->keyType()->isString() && from->isObject() && !from->isClass() &&
         !from->isTuple() && !from->isAnyObject() && !from->isIncomplete() &&
         from->bases().empty() && from->overloads().empty() && !from->isRegExp() &&
         !from->isMatch() && !from->isDate() && !from->isIntl() && !from->isCrypto() &&
         !from->isWeakRef() && !from->isBytes() && !from->isAsyncIterator();
}

const Type *TypesRelations::copiedFieldType(const Field &field, const Type *value) {
  if (field.type->isOptional() && !value->isOptional()) {
    return field.type->inner();
  }

  return field.type;
}

ConversionStep TypesRelations::recordCopyStep(const Type *from, const Type *to) {
  const Type *value = to->valueType();
  auto fields = std::vector<ConversionGoal>();
  for (const Field &field : from->fields()) {
    const auto *type = this->copiedFieldType(field, value);
    if (type->isFunction()) {
      return this->stepFails();
    }
    fields.push_back(ConversionGoal{.from = type, .to = value});
  }

  return this->stepNeeds(std::move(fields));
}

bool TypesRelations::isImplicitlyConvertible(const Type *from, const Type *to) {
  auto first = ConversionGoal{.from = from, .to = to};
  auto tasks = std::vector<ConversionTask>{first};
  while (!tasks.empty()) {
    ConversionTask task = std::move(tasks.back());
    tasks.pop_back();
    auto step = this->taskStep(task);
    if (step.isPossible) {
      tasks.insert(tasks.end(), step.needs.rbegin(), step.needs.rend());
    } else if (!this->canRetryMember(tasks)) {
      return false;
    }
  }

  return true;
}

ConversionStep TypesRelations::taskStep(const ConversionTask &task) {
  switch (task.kind()) {
    case ConversionTaskKind::GOAL:
      return this->conversionStep(task.as<ConversionGoal>()->from, task.as<ConversionGoal>()->to);
    case ConversionTaskKind::MEMBER:
      return this->memberStep(*task.as<MemberGoal>());
    case ConversionTaskKind::CHOICE:
      return this->stepHolds();
  }
  llvm_unreachable("unknown conversion task");
}

ConversionStep TypesRelations::memberGoals(const Type *from, const Type *to) {
  const Type *source = from;
  llvm::ArrayRef<const Type *> members =
      source->isUnion() ? source->members() : llvm::ArrayRef<const Type *>(source);
  auto needs = std::vector<ConversionTask>();
  for (const Type *member : members) {
    needs.push_back(MemberGoal{.from = member, .to = to});
  }

  return ConversionStep{.isPossible = true, .needs = std::move(needs)};
}

ConversionStep TypesRelations::memberStep(const MemberGoal &goal) {
  if (this->types.literals.exactMemberFor(goal.from, goal.to) != nullptr) {
    return this->stepHolds();
  }

  return this->memberTrial(MemberChoice{.from = goal.from, .to = goal.to, .next = 0});
}

ConversionStep TypesRelations::memberTrial(MemberChoice choice) {
  llvm::ArrayRef<const Type *> members = choice.to->members();
  while (choice.next < members.size()) {
    const Type *member = members[choice.next];
    choice.next += 1;
    if (!member->isParam() && !choice.from->isParam()) {
      auto goal = ConversionGoal{.from = choice.from, .to = member};

      return ConversionStep{.isPossible = true, .needs = {goal, choice}};
    }
  }

  return this->stepFails();
}

bool TypesRelations::canRetryMember(std::vector<ConversionTask> &tasks) {
  while (!tasks.empty()) {
    ConversionTask task = std::move(tasks.back());
    tasks.pop_back();
    const MemberChoice *choice = task.as<MemberChoice>();
    if (choice != nullptr && this->canTryNextMember(*choice, tasks)) {
      return true;
    }
  }

  return false;
}

bool TypesRelations::canTryNextMember(const MemberChoice &choice,
                                      std::vector<ConversionTask> &tasks) {
  auto trial = this->memberTrial(choice);
  if (!trial.isPossible) {
    return false;
  }
  tasks.insert(tasks.end(), trial.needs.rbegin(), trial.needs.rend());

  return true;
}

ConversionStep TypesRelations::conversionStep(const Type *from, const Type *to) {
  if (from == to || from->isNever()) {
    return this->stepHolds();
  }
  auto byTarget = this->targetStep(from, to);
  if (byTarget.has_value()) {
    return *byTarget;
  }
  auto bySource = this->sourceStep(from, to);
  if (bySource.has_value()) {
    return *bySource;
  }
  auto plain = this->plainConvertible(from, to);
  if (plain.has_value()) {
    return this->stepDecided(*plain);
  }
  if (this->hasRecordCopyShape(from, to)) {
    return this->recordCopyStep(from, to);
  }
  if (from->isResult() && to->isResult()) {
    return this->resultStep(from, to);
  }

  return this->stepFails();
}

std::optional<ConversionStep> TypesRelations::targetStep(const Type *from, const Type *to) {
  if (to->isUnknown()) {
    return this->stepDecided(!from->isVoid());
  }
  if (to->isAnyObject()) {
    return this->anyObjectStep(from, to);
  }
  if (to->isConstructor() && from->isClassRef()) {
    return this->classRefStep(from, to);
  }
  if (to->isJson()) {
    return this->jsonStep(from, to);
  }
  if (to->isOptional() && from->isUndefined()) {
    return this->stepHolds();
  }
  if (to->isOptional()) {
    return this->stepNeeds({{from->nonOptional(), to->inner()}});
  }
  if (to->isUnion()) {
    return this->memberGoals(from, to);
  }

  return std::nullopt;
}

ConversionStep TypesRelations::anyObjectStep(const Type *from, const Type *to) {
  if (UnknownChecker::isObjectReference(from)) {
    return this->stepHolds();
  }
  if (from->isParam() && from->constraint() != nullptr) {
    return this->stepNeeds({{from->constraint(), to}});
  }

  return this->stepFails();
}

ConversionStep TypesRelations::classRefStep(const Type *from, const Type *to) {
  if (from->classRef() != nullptr && from->classRef()->isGeneric()) {
    return this->stepFails();
  }
  const auto *made = ClassValueChecker::constructedBy(from);
  if (made != nullptr) {
    return this->stepNeeds({{made, to->constructed()}});
  }

  return this->stepDecided(to->constructed()->isAnyObject());
}

std::optional<ConversionStep> TypesRelations::sourceStep(const Type *from, const Type *to) {
  if (from->isUnion()) {
    return this->unionSourceStep(from, to);
  }
  if (to->isPattern()) {
    bool isMatching =
        from->isStringLiteral() && TemplatePatterns::matchesPattern(from->literalText(), to);
    return this->stepDecided(isMatching);
  }
  if (from->isPattern()) {
    return this->stepDecided(to == Type::string());
  }
  if (from->isLiteral() && to->isLiteral()) {
    return this->stepFails();
  }
  if (from->isLiteral()) {
    return this->stepNeeds({{from->literalBase(), to}});
  }
  if (from->isParam() && from->constraint() != nullptr) {
    return this->stepNeeds({{from->constraint(), to}});
  }

  return std::nullopt;
}

ConversionStep TypesRelations::unionSourceStep(const Type *from, const Type *to) {
  if (from->isTagged() || UnionSlots::hasParamMember(from)) {
    return this->stepFails();
  }
  auto members = std::vector<ConversionGoal>();
  for (const Type *member : from->members()) {
    members.push_back(ConversionGoal{.from = member, .to = to});
  }

  return this->stepNeeds(std::move(members));
}

ConversionStep TypesRelations::stepHolds() {
  return ConversionStep{.isPossible = true, .needs = {}};
}

ConversionStep TypesRelations::stepFails() {
  return ConversionStep{.isPossible = false, .needs = {}};
}

ConversionStep TypesRelations::stepDecided(bool isPossible) {
  return ConversionStep{.isPossible = isPossible, .needs = {}};
}

ConversionStep TypesRelations::stepNeeds(std::vector<ConversionGoal> goals) {
  auto needs = std::vector<ConversionTask>(goals.begin(), goals.end());

  return ConversionStep{.isPossible = true, .needs = std::move(needs)};
}

ConversionStep TypesRelations::jsonStep(const Type *from, const Type *to) {
  bool isPlain = from->isString() || from->isNumeric() || from->isBool() || from->isLiteral() ||
                 (from->isArray() && from->inner()->isJson());
  if (isPlain) {
    return this->stepHolds();
  }
  bool isRecord = from->isRecord() && from->keyType()->isString() &&
                  !this->isHoldingWideInteger(from->valueType());
  if (!isRecord) {
    return this->stepFails();
  }

  return this->stepNeeds({{from->valueType()->nonOptional(), to}});
}

ConversionStep TypesRelations::resultStep(const Type *from, const Type *to) {
  auto halves = std::vector<ConversionGoal>{
      ConversionGoal{.from = from->okType(), .to = to->okType()},
      ConversionGoal{.from = from->errType(), .to = to->errType()},
  };
  auto needs = std::vector<ConversionGoal>();
  for (const ConversionGoal &half : halves) {
    bool isSettled = half.from == half.to || half.from->isNever();
    bool hasVoid = half.from->isVoid() || half.to->isVoid();
    if (!isSettled && hasVoid) {
      return this->stepFails();
    }
    if (!isSettled) {
      needs.push_back(half);
    }
  }

  return this->stepNeeds(std::move(needs));
}

std::optional<bool> TypesRelations::plainConvertible(const Type *from, const Type *to) {
  if (from->isArray() && to->isArray()) {
    return WideningRelations::isSameReference(from->inner(), to->inner());
  }
  if (from->isIterator() && to->isIterator()) {
    return from->yielded() == to->yielded() &&
           (from->returned() == to->returned() || to->returned()->isVoid()) &&
           (from->sent() == to->sent() || to->sent()->isVoid());
  }
  if (from->isPromise() || to->isPromise()) {
    return from->isPromise() && to->isPromise() && from->promised()->isNever();
  }
  if (from->isBytes() || to->isBytes()) {
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8;
  }
  if (this->isRuntimeObject(from) || this->isRuntimeObject(to)) {
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 &&
           from->field(MatchMemberTable::getName(MatchMember::INDICES)) == nullptr;
  }
  if (from->isObject() && to->isObject() && from->isSubtypeOf(to)) {
    return true;
  }
  if (GenericWidening::isWidening(from, to)) {
    return true;
  }
  if (this->isSameKindOfShape(from, to)) {
    return this->hasSameFields(from, to);
  }

  return std::nullopt;
}

bool TypesRelations::isRuntimeObject(const Type *type) {
  return type->isMatch() || type->isRegExp() || type->isDate() || type->isIntl() ||
         type->isCrypto() || type->isWeakRef();
}

bool TypesRelations::isSameKindOfShape(const Type *from, const Type *to) {
  bool isPlainPair = from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
                     !from->isAnyObject() && !to->isAnyObject();
  bool isSameLayout =
      from->isTuple() == to->isTuple() && from->fields().size() == to->fields().size();

  return isPlainPair && isSameLayout && from->overloads().empty() && to->overloads().empty();
}

bool TypesRelations::hasSameFields(const Type *from, const Type *to) {
  for (const Field &field : from->fields()) {
    const Field *same = to->field(field.name);
    if (same == nullptr || same->type != field.type) {
      return false;
    }
  }

  return true;
}

const Type *TypesRelations::commonNumericType(const Type *first, const Type *second) {
  if (this->isImplicitlyConvertible(first, second)) {
    return second;
  }
  if (this->isImplicitlyConvertible(second, first)) {
    return first;
  }

  return nullptr;
}

bool TypesRelations::isNumberLike(const Type *type) {
  return type->isNumeric() || type->isDecimal();
}

const Type *TypesRelations::unify(const Type *first, const Type *second) const {
  return this->unifyAs(UnifyForm::WHOLE, first, second);
}

const Type *TypesRelations::unifyAs(UnifyForm form, const Type *first, const Type *second) {
  auto start = EnterUnify{.first = first, .second = second, .form = form};
  auto tasks = std::vector<UnifyTask>{start};
  auto answers = std::vector<const Type *>();
  while (!tasks.empty()) {
    UnifyTask task = tasks.back();
    tasks.pop_back();
    switch (task.kind()) {
      case UnifyTaskKind::ENTER:
        this->enterUnify(*task.as<EnterUnify>(), tasks, answers);
        break;
      case UnifyTaskKind::COLLECT:
        this->collectUnify(*task.as<CollectUnify>(), answers);
        break;
    }
  }

  return answers.back();
}

void TypesRelations::enterUnify(const EnterUnify &enter, std::vector<UnifyTask> &tasks,
                                std::vector<const Type *> &answers) {
  switch (enter.form) {
    case UnifyForm::WHOLE:
      return this->enterWholeUnify(enter.first, enter.second, tasks, answers);
    case UnifyForm::HALF:
      return this->enterHalfUnify(enter.first, enter.second, tasks, answers);
    case UnifyForm::UNION:
      return this->pushUnify(tasks, enter.first, enter.second, UnifyForm::UNION, UnifyForm::WHOLE);
  }
}

void TypesRelations::pushUnify(std::vector<UnifyTask> &tasks, const Type *first, const Type *second,
                               UnifyForm collected, UnifyForm entered) {
  tasks.push_back(CollectUnify{.first = first, .second = second, .form = collected});
  tasks.push_back(EnterUnify{.first = first, .second = second, .form = entered});
}

void TypesRelations::enterHalfUnify(const Type *first, const Type *second,
                                    std::vector<UnifyTask> &tasks,
                                    std::vector<const Type *> &answers) {
  auto shortcut = ResultTypes::halfShortcut(first, second);
  if (shortcut.isDecided) {
    return answers.push_back(shortcut.type);
  }

  return this->pushUnify(tasks, first, second, UnifyForm::HALF, UnifyForm::UNION);
}

void TypesRelations::enterWholeUnify(const Type *first, const Type *second,
                                     std::vector<UnifyTask> &tasks,
                                     std::vector<const Type *> &answers) {
  auto shortcut = this->shortUnify(first, second);
  if (shortcut.isDecided) {
    return answers.push_back(shortcut.type);
  }
  const Type *plainFirst = first->nonOptional();
  const Type *plainSecond = second->nonOptional();
  bool isResultPair =
      plainFirst->isResult() && plainSecond->isResult() && plainFirst != plainSecond;
  if (isResultPair) {
    return this->pushResultUnify(tasks, first, second);
  }
  const Type *base = this->baseUnify(plainFirst, plainSecond);
  const Type *unified = this->optionalLike(first, second, base);

  return answers.push_back(unified);
}

void TypesRelations::pushResultUnify(std::vector<UnifyTask> &tasks, const Type *first,
                                     const Type *second) {
  const Type *firstResult = first->nonOptional();
  const Type *secondResult = second->nonOptional();
  tasks.push_back(CollectUnify{.first = first, .second = second, .form = UnifyForm::WHOLE});
  tasks.push_back(EnterUnify{
      .first = firstResult->errType(),
      .second = secondResult->errType(),
      .form = UnifyForm::HALF,
  });
  tasks.push_back(EnterUnify{
      .first = firstResult->okType(),
      .second = secondResult->okType(),
      .form = UnifyForm::HALF,
  });
}

void TypesRelations::collectUnify(const CollectUnify &collect, std::vector<const Type *> &answers) {
  const Type *collected = this->collectedType(collect, answers);
  answers.push_back(collected);
}

const Type *TypesRelations::collectedType(const CollectUnify &collect,
                                          std::vector<const Type *> &answers) {
  switch (collect.form) {
    case UnifyForm::WHOLE:
      return this->collectedResult(collect, answers);
    case UnifyForm::HALF:
      return this->collectedHalf(answers);
    case UnifyForm::UNION:
      return this->collectedUnion(collect, answers);
  }

  return nullptr;
}

const Type *TypesRelations::collectedResult(const CollectUnify &collect,
                                            std::vector<const Type *> &answers) {
  const Type *error = ChildrenFirst::takeAnswer(answers);
  const Type *value = ChildrenFirst::takeAnswer(answers);
  if (value == nullptr || error == nullptr) {
    return nullptr;
  }
  const auto *result = Type::result(value, error);

  return this->optionalLike(collect.first, collect.second, result);
}

const Type *TypesRelations::collectedHalf(std::vector<const Type *> &answers) {
  const Type *common = ChildrenFirst::takeAnswer(answers);
  if (common == nullptr || common->isError()) {
    return nullptr;
  }

  return common;
}

const Type *TypesRelations::collectedUnion(const CollectUnify &collect,
                                           std::vector<const Type *> &answers) {
  const Type *common = ChildrenFirst::takeAnswer(answers);
  if (common != nullptr) {
    return common;
  }

  return this->types.literals.unionFallback(collect.first, collect.second);
}

const Type *TypesRelations::optionalLike(const Type *first, const Type *second, const Type *base) {
  if (base == nullptr) {
    return nullptr;
  }
  if (first->isOptional() || second->isOptional()) {
    return Type::optional(base);
  }

  return base;
}

UnifyShortcut TypesRelations::shortUnify(const Type *first, const Type *second) {
  if (first == second) {
    return this->decided(first);
  }
  if (first->isError() || second->isError()) {
    return this->decided(Type::error());
  }
  if (first->isNever()) {
    return this->decided(second);
  }
  if (second->isNever()) {
    return this->decided(first);
  }
  if (first->isVoid() || second->isVoid()) {
    return this->decided(nullptr);
  }
  if (first->isUndefined()) {
    return this->decided(Type::optional(second));
  }
  if (second->isUndefined()) {
    return this->decided(Type::optional(first));
  }
  if (first->isUnknown() || second->isUnknown()) {
    return this->decided(Type::unknown());
  }
  const Type *plainFirst = first->nonOptional();
  const Type *plainSecond = second->nonOptional();
  bool hasChoice = plainFirst->isLiteral() || plainSecond->isLiteral() || plainFirst->isUnion() ||
                   plainSecond->isUnion();
  if (hasChoice) {
    const Type *wider = this->commonNumericType(plainFirst, plainSecond);
    const Type *unified = this->optionalLike(first, second, wider);

    return this->decided(unified);
  }
  if (plainFirst->isString() && plainSecond->isString()) {
    const Type *unified = this->optionalLike(first, second, Type::string());

    return this->decided(unified);
  }

  return UnifyShortcut{.isDecided = false, .type = nullptr};
}

UnifyShortcut TypesRelations::decided(const Type *type) {
  return UnifyShortcut{.isDecided = true, .type = type};
}

const Type *TypesRelations::baseUnify(const Type *first, const Type *second) {
  if (first == second) {
    return first;
  }
  const Type *related = this->widerOfRelated(first, second);
  if (related != nullptr) {
    return related;
  }
  const Type *json = this->widerJson(first, second);
  if (json != nullptr) {
    return json;
  }
  const Type *parent = this->commonParent(first, second);
  if (parent != nullptr) {
    return parent;
  }
  const Type *array = this->widerArray(first, second);
  if (array != nullptr) {
    return array;
  }
  const Type *promise = this->settledPromise(first, second);
  if (promise != nullptr) {
    return promise;
  }
  const Type *constructor = this->commonConstructor(first, second);
  if (constructor != nullptr) {
    return constructor;
  }

  return this->commonAnyObject(first, second);
}

const Type *TypesRelations::widerOfRelated(const Type *first, const Type *second) {
  bool isNumbers = this->isNumberLike(first) && this->isNumberLike(second);
  bool isObjects = first->isObject() && second->isObject();
  bool isIterators = first->isIterator() && second->isIterator();
  if (!isNumbers && !isObjects && !isIterators) {
    return nullptr;
  }

  return this->commonNumericType(first, second);
}

const Type *TypesRelations::widerJson(const Type *first, const Type *second) {
  if (!first->isJson() && !second->isJson()) {
    return nullptr;
  }

  return this->commonNumericType(second, first);
}

const Type *TypesRelations::commonParent(const Type *first, const Type *second) {
  if (!first->isClass() || !second->isClass()) {
    return nullptr;
  }

  return ClassLinks::commonClass(first, second);
}

const Type *TypesRelations::widerArray(const Type *first, const Type *second) {
  if (!first->isArray() || !second->isArray()) {
    return nullptr;
  }

  return this->commonNumericType(first, second);
}

const Type *TypesRelations::settledPromise(const Type *first, const Type *second) {
  if (!first->isPromise() || !second->isPromise()) {
    return nullptr;
  }
  if (first->promised()->isNever()) {
    return second;
  }
  if (second->promised()->isNever()) {
    return first;
  }

  return nullptr;
}

const Type *TypesRelations::commonConstructor(const Type *first, const Type *second) {
  if (!first->isClassRef() || !second->isClassRef()) {
    return nullptr;
  }
  const Type *firstMade = ClassValueChecker::constructedBy(first);
  const Type *secondMade = ClassValueChecker::constructedBy(second);
  const Type *made = this->commonInstance(firstMade, secondMade);
  if (made == nullptr) {
    return Type::constructorOf(Type::anyObject());
  }

  return Type::constructorOf(made);
}

const Type *TypesRelations::commonInstance(const Type *first, const Type *second) {
  if (first == nullptr || second == nullptr) {
    return nullptr;
  }
  const Type *wider = this->commonNumericType(first, second);
  if (wider != nullptr) {
    return wider;
  }

  return ClassLinks::commonClass(first, second);
}

const Type *TypesRelations::commonAnyObject(const Type *first, const Type *second) {
  bool hasAnyObject = first->isAnyObject() || second->isAnyObject();
  bool isReferences =
      UnknownChecker::isObjectReference(first) && UnknownChecker::isObjectReference(second);
  if (hasAnyObject && isReferences) {
    return Type::anyObject();
  }

  return nullptr;
}

const Type *TypesRelations::comparisonType(const Type *first, const Type *second) const {
  if (first->isVoid() || second->isVoid()) {
    return nullptr;
  }
  const Type *common = this->unify(first, second);
  if (common == nullptr || common->isError()) {
    return common;
  }
  if (!this->isComparable(common->nonOptional())) {
    return nullptr;
  }

  return common;
}

bool TypesRelations::isComparable(const Type *base) const {
  bool isPlainUnion = base->isUnion() && !UnionSlots::hasParamMember(base);

  return base->isNumeric() || base->isDecimal() || base->isBool() || base->isString() ||
         base->isArray() || base->isFunction() || base->isObject() || base->isIterator() ||
         base->isJson() || base->isDict() || base->isUnknown() || base->isClassRef() ||
         base->isLiteral() || isPlainUnion;
}

bool TypesRelations::isMatchingTemplate(const std::u16string &text,
                                        llvm::ArrayRef<const Type *> parts, InferMatch *inference) {
  auto texts = std::vector<std::u16string>();
  auto holes = std::vector<const Type *>();
  for (size_t index = 0; index < parts.size(); ++index) {
    bool isText = index % 2 == 0;
    if (isText) {
      texts.push_back(parts[index]->literalText());
    } else {
      holes.push_back(parts[index]);
    }
  }
  auto pieces = std::vector<std::u16string>();
  if (!TemplatePatterns::splitByTemplate(text, texts, pieces)) {
    return false;
  }
  for (size_t index = 0; index < holes.size(); ++index) {
    if (!this->isPieceFittingHole(pieces[index], holes[index], inference)) {
      return false;
    }
  }

  return true;
}

bool TypesRelations::isPieceFittingHole(const std::u16string &piece, const Type *hole,
                                        InferMatch *inference) {
  if (!TypeQueries::mentionsParam(hole)) {
    return TemplatePatterns::holeAccepts(hole, piece);
  }
  const Type *pieceType = this->pieceType(piece, hole, inference);
  if (pieceType == nullptr) {
    return false;
  }

  return this->isExtendingType(pieceType, hole, inference);
}

const Type *TypesRelations::pieceType(const std::u16string &piece, const Type *hole,
                                      const InferMatch *inference) const {
  if (!this->isNumberHole(hole, inference)) {
    return Type::stringLiteral(piece);
  }
  double value = 0;
  if (!TemplatePatterns::numberString(piece, value)) {
    return nullptr;
  }
  const Type *number = this->numberLiteral(value);
  if (number->name() != ToUtf8::toUtf8(piece)) {
    return hole->constraint()->nonOptional();
  }

  return number;
}

bool TypesRelations::isNumberHole(const Type *hole, const InferMatch *inference) const {
  bool isInferred = inference != nullptr && llvm::is_contained(inference->params, hole);
  if (!isInferred || hole->constraint() == nullptr) {
    return false;
  }

  return hole->constraint()->nonOptional()->isNumeric();
}

const Type *TypesRelations::numberLiteral(double value) const {
  bool isWhole = value == std::trunc(value) && std::fabs(value) <= kMaxWholeNumber;
  if (isWhole) {
    return Type::numberLiteral(value, Type::intTy());
  }

  return Type::numberLiteral(value, Type::doubleTy());
}

bool TypesRelations::isFittingConditional(const Type *from, const Type *to) {
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

bool TypesRelations::isMentioningParamBesides(const Type *type,
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

bool TypesRelations::isGoalFitting(const FitsGoal &goal, std::vector<FitsStep> &steps) {
  if (goal.depth > kMaxFitsDepth || goal.to->typeOp() != TypeOp::Conditional) {
    return false;
  }
  if (goal.from->isUnion()) {
    for (const Type *member : llvm::reverse(goal.from->members())) {
      steps.push_back(FitsGoal{.from = member, .to = goal.to, .depth = goal.depth + 1});
    }
    return true;
  }
  auto branches = this->types.context.findConditionalBranches(goal.to);
  auto choice = FitsChoice{
      .from = goal.from,
      .depth = goal.depth,
      .branches = std::move(branches),
      .next = 0,
  };

  return this->canTryFitsAlternative(std::move(choice), steps);
}

bool TypesRelations::canRetryFits(std::vector<FitsStep> &steps) {
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

bool TypesRelations::isPlainParam(const Type *type) const {
  return type->kind() == Type::Kind::Param && type->typeOp() == TypeOp::None;
}

std::vector<const Type *>
TypesRelations::otherParamParts(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) const {
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

bool TypesRelations::canTryFitsAlternative(FitsChoice choice, std::vector<FitsStep> &steps) {
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

std::vector<const Type *> TypesRelations::operandParts(const Type *type) const {
  llvm::ArrayRef<const Type *> operands = type->operands();
  if (type->typeOp() == TypeOp::Conditional) {
    return std::vector<const Type *>(operands.begin() + 1, operands.end());
  }

  return std::vector<const Type *>(operands.begin(), operands.end());
}

std::vector<const Type *> TypesRelations::functionParts(const Type *function) const {
  auto parts = std::vector<const Type *>{function->ret()};
  parts.insert(parts.end(), function->params().begin(), function->params().end());

  return parts;
}

std::vector<const Type *>
TypesRelations::objectParamParts(const Type *type,
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

bool TypesRelations::isFittingBy(FitsWay way, const Type *from, const Type *branch) {
  switch (way) {
    case FitsWay::CONVERSION:
      return this->isImplicitlyConvertible(from, branch);
    case FitsWay::NESTED:
      return false;
    case FitsWay::INFERENCE:
      return this->isExtendingWithParams(from, branch);
  }

  return false;
}

bool TypesRelations::isExtendingWithParams(const Type *from, const Type *branch) {
  auto params = this->plainParamsIn(branch);
  if (params.empty()) {
    return false;
  }
  auto unbound = std::vector<const Type *>(params.size(), nullptr);
  auto match = InferMatch{.params = params, .bound = unbound};

  return this->isExtendingType(from, branch, &match);
}

std::vector<const Type *> TypesRelations::plainParamsIn(const Type *type) const {
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

bool TypesRelations::isFirstMention(const Type *type,
                                    llvm::SmallPtrSetImpl<const Type *> &seen) const {
  if (type == nullptr || !TypeQueries::mentionsParam(type)) {
    return false;
  }
  auto [_, isFirstVisit] = seen.insert(type);

  return isFirstVisit;
}

std::vector<const Type *> TypesRelations::paramParts(const Type *type) const {
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

std::vector<const Type *> TypesRelations::objectParts(const Type *object) const {
  auto parts = std::vector<const Type *>(object->typeArgs().begin(), object->typeArgs().end());
  for (const Field &field : object->fields()) {
    parts.push_back(field.type);
  }

  return parts;
}

const Type *TypesRelations::knownFunctionType(const FuncSymbol *function) const {
  const auto *returned = this->knownReturnType(function);

  return Type::function(function->params, function->minArgs, function->hasRest, returned,
                        function->hasThis);
}

const Type *TypesRelations::knownReturnType(const FuncSymbol *function) const {
  if (function->hasReturnType()) {
    return function->getKnownReturnType();
  }
  if (function->inferring && function->assumed != nullptr) {
    return function->assumed;
  }

  return Type::error();
}

FuncSymbol *TypesRelations::unknownMethod(const Type *type, const ClassDecl *classDecl,
                                          llvm::StringRef name) const {
  if (type->field(name) != nullptr || classDecl == nullptr) {
    return nullptr;
  }
  auto methods = classDecl->findMethodsNamed(name);
  bool isPlainMethod =
      !methods.empty() && methods.back()->sym != nullptr && methods.back()->typeParams.empty();
  if (!isPlainMethod || methods.back()->sym->hasReturnType()) {
    return nullptr;
  }

  return methods.back()->sym;
}

std::vector<FuncSymbol *> TypesRelations::findExtendsNeeds(const Type *type, const Type *pattern,
                                                           const InferMatch *inference) {
  auto needs = std::vector<FuncSymbol *>();
  this->neededFunctions = &needs;
  if (inference == nullptr) {
    this->isExtendingType(type, pattern, nullptr);
  } else {
    auto trial = *inference;
    this->isExtendingType(type, pattern, &trial);
  }
  this->neededFunctions = nullptr;

  return needs;
}

const Type *TypesRelations::commonBaseWith(const Type *type, llvm::StringRef name) const {
  if (!this->isAllObjects(type)) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  for (const Type *candidate : this->baseChain(members.front())) {
    if (this->isCommonBase(candidate, members) && this->hasMember(candidate, name)) {
      return candidate;
    }
  }

  return nullptr;
}

bool TypesRelations::isAllObjects(const Type *type) const {
  for (const Type *member : type->members()) {
    if (!member->isObject()) {
      return false;
    }
  }

  return true;
}

std::vector<const Type *> TypesRelations::baseChain(const Type *type) const {
  auto chain = std::vector<const Type *>{type};
  for (size_t index = 0; index < chain.size(); ++index) {
    for (const Type *base : chain[index]->bases()) {
      if (!llvm::is_contained(chain, base)) {
        chain.push_back(base);
      }
    }
  }

  return chain;
}

bool TypesRelations::isCommonBase(const Type *candidate,
                                  llvm::ArrayRef<const Type *> members) const {
  bool isOneOfSeveral = llvm::is_contained(members, candidate) && members.size() > 1;
  if (isOneOfSeveral) {
    return false;
  }
  for (const Type *member : members.drop_front()) {
    if (!member->isSubtypeOf(candidate)) {
      return false;
    }
  }

  return true;
}

bool TypesRelations::hasMember(const Type *candidate, llvm::StringRef name) const {
  const ClassDecl *classDecl = candidate->classDecl();
  if (classDecl == nullptr) {
    return candidate->field(name) != nullptr || candidate->overload(name) != nullptr;
  }
  if (classDecl->findField(name) != nullptr) {
    return true;
  }
  auto methods = classDecl->findMethodsNamed(name);

  return !methods.empty();
}

}
