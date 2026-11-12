#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

Inference::Inference(llvm::ArrayRef<const Type *> typeParams, const AliasStore &aliases,
                     TypeOf elementOfType)
    : params(typeParams),
      candidates(typeParams.size()),
      weakCandidates(typeParams.size()),
      literalCandidates(typeParams.size()),
      undefinedSeen(typeParams.size(), false),
      origins(aliases),
      elementOf(std::move(elementOfType)) {}

const Type *Inference::paramsAsList(const Type *functionType, size_t from) {
  llvm::ArrayRef<const Type *> params = functionType->params();
  size_t fixed = params.size() - (functionType->hasRest() ? 1 : 0);
  auto elements = std::vector<const Type *>();
  for (size_t index = from; index < fixed; ++index) {
    elements.push_back(params[index]);
  }
  size_t min = functionType->minArgs() > from ? functionType->minArgs() - from : 0;

  return Type::argumentList(elements, std::min(min, elements.size()),
                            functionType->hasRest() ? params.back() : nullptr);
}

const Type *Inference::listSlice(const Type *list, size_t begin, size_t end, bool last) {
  auto elements = std::vector<const Type *>();
  for (size_t index = begin; index < end && index < list->fields().size(); ++index) {
    elements.push_back(list->fields()[index].type);
  }
  size_t min = elements.size();
  if (last) {
    min = list->tupleMin() > begin ? std::min(list->tupleMin() - begin, elements.size()) : 0;
  }

  return Type::argumentList(elements, min, last ? list->tupleRest() : nullptr);
}

bool Inference::hasDeferred() const {
  return !this->deferred.empty();
}

void Inference::setAsyncOf(TypeOf asyncOfType) {
  this->asyncOf = std::move(asyncOfType);
}

bool Inference::hasSeenUndefined(size_t index) const {
  return this->undefinedSeen[index];
}

void Inference::adoptCandidates(size_t index, const Inference &other) {
  this->candidates[index] = other.candidates[index];
  this->weakCandidates[index].clear();
  this->literalCandidates[index].clear();
  this->undefinedSeen[index] = other.undefinedSeen[index];
}

void Inference::markArgument(ArgumentStrength strength, bool isIntLiteralArgument) {
  this->isWeak = strength == ArgumentStrength::WEAK;
  this->isIntLiteral = isIntLiteralArgument;
}

void Inference::forgetArgumentMarks() {
  this->isWeak = false;
  this->isIntLiteral = false;
}

const std::vector<const Type *> &Inference::getCandidates(size_t index) const {
  return this->candidates[index];
}

const std::vector<const Type *> &Inference::getWeakCandidates(size_t index) const {
  return this->weakCandidates[index];
}

const std::vector<const Type *> &Inference::getLiteralCandidates(size_t index) const {
  return this->literalCandidates[index];
}

std::vector<InferStep> Inference::inferPair(const Type *pattern, const Type *actual) {
  if (actual->isError()) {
    return {};
  }
  auto param = this->paramIndex(pattern);
  if (actual->isVoid() && param.has_value()) {
    this->candidates[*param].push_back(actual);
  }
  if (actual->isVoid()) {
    return {};
  }
  if (param.has_value()) {
    return this->inferParam(*param, pattern, actual);
  }
  if (!GenericCheckHelpers::mentionsAnyOf(pattern, this->params)) {
    return {};
  }

  return this->inferInside(pattern, actual);
}

std::optional<size_t> Inference::paramIndex(const Type *type) const {
  auto at = llvm::find(this->params, type);
  if (at == this->params.end()) {
    return std::nullopt;
  }

  return size_t(at - this->params.begin());
}

std::vector<InferStep> Inference::inferParam(size_t index, const Type *pattern,
                                             const Type *actual) {
  if (actual->isUndefined()) {
    this->undefinedSeen[index] = true;

    return {};
  }
  const Type *element = pattern->iterated();
  bool isWeakList = element != nullptr && this->isWeak && actual->isArray() &&
                    !TypeQueries::mentionsParam(element);
  const Type *candidate = isWeakList ? Type::array(element) : actual;
  this->candidatesFor(index).push_back(candidate);
  if (element == nullptr || this->elementOf == nullptr) {
    return {};
  }
  const Type *given = this->elementOf(candidate);
  if (given == nullptr) {
    return {};
  }

  return {InferPair{.pattern = element, .actual = given}};
}

std::vector<const Type *> &Inference::candidatesFor(size_t index) {
  if (this->isIntLiteral) {
    return this->literalCandidates[index];
  }
  if (this->isWeak) {
    return this->weakCandidates[index];
  }

  return this->candidates[index];
}

std::vector<InferStep> Inference::inferInside(const Type *pattern, const Type *actual) {
  switch (pattern->kind()) {
    case Type::Kind::Optional:
      if (actual->isUndefined()) {
        return {};
      }
      return {InferPair{pattern->inner(), actual->nonOptional()}};
    case Type::Kind::Array:
      if (!actual->isArray()) {
        return {};
      }
      return {InferPair{pattern->inner(), actual->inner()}};
    case Type::Kind::Result:
      if (!actual->isResult()) {
        return {};
      }
      return {InferHeld{pattern->okType(), actual->okType()},
              InferPair{pattern->errType(), actual->errType()}};
    case Type::Kind::Function:
      return this->inferFunction(pattern, actual);
    case Type::Kind::Iterator:
      if (!actual->isIterator()) {
        return {};
      }
      return Inference::iteratorParts(pattern, actual);
    case Type::Kind::Dict:
      return Inference::inferDict(pattern, actual);
    case Type::Kind::Promise:
      if (!actual->isPromise()) {
        return {};
      }
      return {InferHeld{pattern->promised(), actual->promised()}};
    case Type::Kind::ClassRef:
      return Inference::inferConstructor(pattern, actual);
    case Type::Kind::Object:
      return this->inferObject(pattern, actual);
    case Type::Kind::Union:
      return this->inferUnion(pattern, actual);
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
    case Type::Kind::Json:
    case Type::Kind::Literal:
    case Type::Kind::Unknown:
      return {};
  }

  return {};
}

std::vector<InferStep> Inference::inferFunction(const Type *pattern, const Type *actual) {
  if (!actual->isFunction()) {
    return {};
  }
  auto inner = std::vector<InferStep>();
  llvm::ArrayRef<const Type *> patternParams = pattern->params();
  llvm::ArrayRef<const Type *> actualParams = actual->params();
  const Type *last = pattern->hasRest() ? patternParams.back() : nullptr;
  bool isList = last != nullptr && (last->isListParam() || ArgListChecker::isSpreadingLists(last));
  size_t fixed = isList ? patternParams.size() - 1 : patternParams.size();
  size_t shared = std::min(fixed, actualParams.size());
  for (size_t index = 0; index < shared; ++index) {
    inner.push_back(InferPair{.pattern = patternParams[index], .actual = actualParams[index]});
  }
  size_t actualFixed = actualParams.size() - (actual->hasRest() ? 1 : 0);
  if (isList && actualFixed >= fixed) {
    const auto *rest = Inference::paramsAsList(actual, fixed);
    inner.push_back(InferPair{.pattern = patternParams.back(), .actual = rest});
  }
  auto returned = this->paramIndex(pattern->ret());
  if (returned.has_value() && actual->ret()->isVoid()) {
    inner.push_back(AddCandidate{*returned, actual->ret()});
  } else if (!pattern->ret()->isVoid()) {
    inner.push_back(InferPair{pattern->ret(), actual->ret()});
  }

  return inner;
}

std::vector<InferStep> Inference::iteratorParts(const Type *pattern, const Type *actual) {
  return {InferPair{pattern->yielded(), actual->yielded()},
          InferPair{pattern->returned(), actual->returned()},
          InferPair{pattern->sent(), actual->sent()}};
}

std::vector<InferStep> Inference::inferDict(const Type *pattern, const Type *actual) {
  bool isPlainObject = actual->isObject() && !actual->isClass() && !actual->isTuple();
  if (pattern->isRecord() && isPlainObject) {
    return Inference::recordFields(pattern, actual);
  }
  if (!actual->isDict() || actual->dictKind() != pattern->dictKind()) {
    return {};
  }

  return {InferPair{pattern->keyType(), actual->keyType()},
          InferPair{pattern->valueType(), actual->valueType()}};
}

std::vector<InferStep> Inference::recordFields(const Type *pattern, const Type *actual) {
  auto inner = std::vector<InferStep>();
  for (const Field &field : actual->fields()) {
    inner.push_back(InferPair{.pattern = pattern->valueType(), .actual = field.type});
  }

  return inner;
}

std::vector<InferStep> Inference::inferConstructor(const Type *pattern, const Type *actual) {
  if (!pattern->isConstructor() || !actual->isClassRef()) {
    return {};
  }
  const Type *made = actual->isConstructor() ? actual->constructed() : actual->classRef()->type;

  return {InferPair{pattern->constructed(), made}};
}

std::vector<InferStep> Inference::inferObject(const Type *pattern, const Type *actual) {
  if (pattern->isAsyncIterator()) {
    return this->inferAsyncIterator(pattern, actual);
  }
  if (!actual->isObject()) {
    return {};
  }
  bool isFirstVisit = this->visited.insert({pattern, actual}).second;
  if (!isFirstVisit) {
    return {};
  }
  if (ArgListChecker::isSpreadingLists(pattern) && actual->isTuple()) {
    return {InferList{pattern, actual, nullptr}};
  }
  const ClassDecl *classDecl = pattern->classDecl();
  if (classDecl != nullptr && !pattern->typeArgs().empty()) {
    return Inference::inferClassArgs(pattern, classDecl, actual);
  }
  const auto *origin = this->origins.findAliasOrigin(pattern);
  std::optional<std::vector<InferStep>> aliased =
      origin != nullptr ? this->inferAliasArgs(origin, actual) : std::nullopt;
  if (aliased.has_value()) {
    return *aliased;
  }

  return Inference::inferFields(pattern, actual);
}

std::vector<InferStep> Inference::inferAsyncIterator(const Type *pattern, const Type *actual) {
  const Type *given = actual;
  if (!actual->isAsyncIterator() && this->asyncOf != nullptr) {
    const Type *converted = this->asyncOf(actual);
    if (converted != nullptr) {
      given = converted;
    }
  }
  if (!given->isAsyncIterator()) {
    return {};
  }

  return Inference::iteratorParts(pattern, given);
}

std::vector<InferStep> Inference::inferClassArgs(const Type *pattern, const ClassDecl *classDecl,
                                                 const Type *actual) {
  const ClassDecl *generic = classDecl->generic() != nullptr ? classDecl->generic() : classDecl;
  const auto *found = GenericClassChecker::genericInstanceOf(actual, generic);
  if (found == nullptr) {
    return {};
  }

  return Inference::pairArgs(pattern->typeArgs(), found->typeArgs);
}

std::optional<std::vector<InferStep>> Inference::inferAliasArgs(const AliasOrigin *origin,
                                                                const Type *actual) {
  std::vector<const Type *> queue{actual};
  for (size_t index = 0; index < queue.size(); ++index) {
    const Type *type = queue[index];
    const auto *other = this->origins.findAliasOrigin(type);
    if (other != nullptr && other->alias == origin->alias) {
      return Inference::pairArgs(origin->args, other->args);
    }
    queue.insert(queue.end(), type->bases().begin(), type->bases().end());
  }

  return std::nullopt;
}

std::vector<InferStep> Inference::pairArgs(llvm::ArrayRef<const Type *> want,
                                           llvm::ArrayRef<const Type *> have) {
  auto inner = std::vector<InferStep>();
  size_t shared = std::min(want.size(), have.size());
  for (size_t index = 0; index < shared; ++index) {
    inner.push_back(InferPair{.pattern = want[index], .actual = have[index]});
  }

  return inner;
}

std::vector<InferStep> Inference::inferFields(const Type *pattern, const Type *actual) {
  auto inner = std::vector<InferStep>();
  for (const Field &field : pattern->fields()) {
    const auto *own = actual->field(field.name);
    if (own != nullptr) {
      inner.push_back(InferPair{.pattern = field.type, .actual = own->type});
    }
  }

  return inner;
}

std::vector<InferStep> Inference::inferUnion(const Type *pattern, const Type *actual) {
  auto open = std::vector<const Type *>();
  auto fixed = std::vector<const Type *>();
  for (const Type *member : pattern->members()) {
    if (GenericCheckHelpers::mentionsAnyOf(member, this->params)) {
      open.push_back(member);
    } else {
      fixed.push_back(member);
    }
  }
  if (open.size() > 1) {
    return Inference::inferGenericMember(open, actual);
  }
  if (open.empty()) {
    return {};
  }
  auto rest = Inference::membersOutside(actual->nonOptional(), fixed);
  if (rest.empty()) {
    return {};
  }

  return {InferPair{open.front(), Type::unionOf(rest)}};
}

std::vector<InferStep> Inference::inferGenericMember(llvm::ArrayRef<const Type *> open,
                                                     const Type *actual) {
  const Type *value = actual->nonOptional();
  for (const Type *member : open) {
    if (Inference::isInstanceOfMember(value, member)) {
      return {InferPair{.pattern = member, .actual = value}};
    }
  }

  return {};
}

bool Inference::isInstanceOfMember(const Type *value, const Type *member) {
  const ClassDecl *classDecl = member->classDecl();
  if (classDecl == nullptr || member->typeArgs().empty()) {
    return false;
  }
  const ClassDecl *generic = classDecl->generic() != nullptr ? classDecl->generic() : classDecl;

  return GenericClassChecker::genericInstanceOf(value, generic) != nullptr;
}

std::vector<const Type *> Inference::membersOutside(const Type *value,
                                                    llvm::ArrayRef<const Type *> fixed) {
  llvm::ArrayRef<const Type *> members =
      value->isUnion() ? value->members() : llvm::ArrayRef<const Type *>(value);
  auto rest = std::vector<const Type *>();
  for (const Type *member : members) {
    if (!llvm::is_contained(fixed, member)) {
      rest.push_back(member);
    }
  }

  return rest;
}

std::vector<InferStep> Inference::inferHeld(const Type *pattern, const Type *actual) {
  auto param = this->paramIndex(pattern);
  if (param.has_value() && actual->isVoid()) {
    auto &found = this->isWeak ? this->weakCandidates : this->candidates;
    found[*param].push_back(actual);

    return {};
  }

  return {InferPair{.pattern = pattern, .actual = actual}};
}

std::vector<InferStep> Inference::inferList(const Type *pattern, const Type *actual,
                                            const std::vector<const Type *> *bound) {
  auto lengths = this->listLengths(pattern, bound);
  if (lengths.isAmbiguous && bound == nullptr) {
    this->deferred.push_back({pattern, actual});
  }
  if (lengths.isAmbiguous || !this->fitListLengths(lengths, pattern, actual)) {
    return {};
  }

  return this->listPairs(pattern, actual, lengths.sizes);
}

Inference::ListLengths Inference::listLengths(const Type *pattern,
                                              const std::vector<const Type *> *bound) const {
  llvm::ArrayRef<Field> parts = pattern->fields();
  auto lengths = ListLengths{
      .sizes = std::vector<int>(parts.size(), 1),
      .known = 0,
      .unknown = -1,
      .isAmbiguous = false,
  };
  for (size_t index = 0; index < parts.size() && !lengths.isAmbiguous; ++index) {
    this->addListLength(parts[index].type, index, bound, lengths);
  }

  return lengths;
}

void Inference::addListLength(const Type *part, size_t index,
                              const std::vector<const Type *> *bound, ListLengths &lengths) const {
  if (!part->isListParam()) {
    lengths.known += 1;
    return;
  }
  const Type *value = this->boundValue(part, bound);
  if (Inference::isFixedList(value)) {
    lengths.sizes[index] = int(value->fields().size());
    lengths.known += value->fields().size();
    return;
  }
  if (lengths.unknown >= 0) {
    lengths.isAmbiguous = true;
    return;
  }
  lengths.unknown = int(index);
}

const Type *Inference::boundValue(const Type *param, const std::vector<const Type *> *bound) const {
  auto position = size_t(llvm::find(this->params, param) - this->params.begin());
  if (bound == nullptr || position >= bound->size()) {
    return nullptr;
  }

  return (*bound)[position];
}

bool Inference::isFixedList(const Type *value) {
  if (value == nullptr || !value->isTuple() || value->tupleRest() != nullptr) {
    return false;
  }

  return value->tupleMin() == value->fields().size();
}

bool Inference::fitListLengths(ListLengths &lengths, const Type *pattern,
                               const Type *actual) const {
  size_t count = actual->fields().size();
  int lastIndex = int(pattern->fields().size()) - 1;
  bool isRestMisplaced = actual->tupleRest() != nullptr && lengths.unknown != lastIndex;
  if (lengths.known > count || isRestMisplaced) {
    return false;
  }
  if (lengths.unknown >= 0) {
    lengths.sizes[size_t(lengths.unknown)] = int(count - lengths.known);
    return true;
  }

  return lengths.known == count;
}

std::vector<InferStep> Inference::listPairs(const Type *pattern, const Type *actual,
                                            llvm::ArrayRef<int> sizes) const {
  auto inner = std::vector<InferStep>();
  llvm::ArrayRef<Field> parts = pattern->fields();
  size_t count = actual->fields().size();
  size_t at = 0;
  for (size_t index = 0; index < parts.size(); ++index) {
    const Type *part = parts[index].type;
    auto length = size_t(sizes[index]);
    if (part->isListParam()) {
      bool isLast = index + 1 == parts.size();
      const auto *slice = Inference::listSlice(actual, at, at + length, isLast);
      inner.push_back(InferPair{.pattern = part, .actual = slice});
    } else if (at < count) {
      inner.push_back(InferPair{.pattern = part, .actual = actual->fields()[at].type});
    }
    at += length;
  }

  return inner;
}

void Inference::from(const Type *pattern, const Type *actual) {
  this->runInference(InferPair{.pattern = pattern, .actual = actual});
}

void Inference::fromHeld(const Type *pattern, const Type *actual) {
  this->runInference(InferHeld{.pattern = pattern, .actual = actual});
}

void Inference::matchList(const Type *pattern, const Type *actual,
                          const std::vector<const Type *> *bound) {
  this->runInference(InferList{.pattern = pattern, .actual = actual, .bound = bound});
}

void Inference::runInference(InferStep first) {
  std::vector<InferStep> steps = {first};
  while (!steps.empty()) {
    InferStep step = steps.back();
    steps.pop_back();
    auto inner = this->applyInferStep(step);
    steps.insert(steps.end(), inner.rbegin(), inner.rend());
  }
}

std::vector<InferStep> Inference::applyInferStep(const InferStep &step) {
  switch (step.kind()) {
    case InferStepKind::PAIR:
      return this->inferPair(step.as<InferPair>()->pattern, step.as<InferPair>()->actual);
    case InferStepKind::HELD:
      return this->inferHeld(step.as<InferHeld>()->pattern, step.as<InferHeld>()->actual);
    case InferStepKind::LIST:
      return this->inferStepList(*step.as<InferList>());
    case InferStepKind::CANDIDATE:
      return this->addCandidate(*step.as<AddCandidate>());
  }

  return {};
}

std::vector<InferStep> Inference::inferStepList(const InferList &list) {
  return this->inferList(list.pattern, list.actual, list.bound);
}

std::vector<InferStep> Inference::addCandidate(const AddCandidate &candidate) {
  this->candidates[candidate.index].push_back(candidate.type);

  return {};
}

void Inference::retry(const std::vector<const Type *> &bound) {
  std::vector<std::pair<const Type *, const Type *>> waiting = std::move(this->deferred);
  this->deferred.clear();
  for (auto [pattern, actual] : waiting) {
    this->matchList(pattern, actual, &bound);
  }
}

}
