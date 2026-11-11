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

const Type *Inference::paramsAsList(const Type *fn, size_t from) {
  llvm::ArrayRef<const Type *> params = fn->params();
  size_t fixed = params.size() - (fn->hasRest() ? 1 : 0);
  std::vector<const Type *> elements;
  for (size_t i = from; i < fixed; ++i) {
    elements.push_back(params[i]);
  }
  size_t min = fn->minArgs() > from ? fn->minArgs() - from : 0;

  return Type::argumentList(elements, std::min(min, elements.size()),
                            fn->hasRest() ? params.back() : nullptr);
}

const Type *Inference::listSlice(const Type *list, size_t begin, size_t end, bool last) {
  std::vector<const Type *> elements;
  for (size_t i = begin; i < end && i < list->fields().size(); ++i) {
    elements.push_back(list->fields()[i].type);
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

bool Inference::hasSeenUndefined(size_t k) const {
  return this->undefinedSeen[k];
}

void Inference::adoptCandidates(size_t k, const Inference &other) {
  this->candidates[k] = other.candidates[k];
  this->weakCandidates[k].clear();
  this->literalCandidates[k].clear();
  this->undefinedSeen[k] = other.undefinedSeen[k];
}

void Inference::markArgument(bool weakArgument, bool intLiteralArgument) {
  this->weak = weakArgument;
  this->intLiteral = intLiteralArgument;
}

const std::vector<const Type *> &Inference::getCandidates(size_t k) const {
  return this->candidates[k];
}

const std::vector<const Type *> &Inference::getWeakCandidates(size_t k) const {
  return this->weakCandidates[k];
}

const std::vector<const Type *> &Inference::getLiteralCandidates(size_t k) const {
  return this->literalCandidates[k];
}

std::vector<InferStep> Inference::inferPair(const Type *pattern, const Type *actual) {
  if (actual->isError()) {
    return {};
  }
  std::optional<size_t> param = this->paramIndex(pattern);
  if (actual->isVoid()) {
    if (param.has_value()) {
      this->candidates[*param].push_back(actual);
    }
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

std::vector<InferStep> Inference::inferParam(size_t k, const Type *pattern, const Type *actual) {
  if (actual->isUndefined()) {
    this->undefinedSeen[k] = true;
    return {};
  }
  const Type *element = pattern->iterated();
  const Type *candidate = actual;
  if (element != nullptr && this->weak && actual->isArray() &&
      !TypeQueries::mentionsParam(element)) {
    candidate = Type::array(element);
  }
  this->candidatesFor(k).push_back(candidate);
  if (element == nullptr || this->elementOf == nullptr) {
    return {};
  }
  const Type *given = this->elementOf(candidate);
  if (given == nullptr) {
    return {};
  }

  return {InferPair{element, given}};
}

std::vector<const Type *> &Inference::candidatesFor(size_t k) {
  if (this->intLiteral) {
    return this->literalCandidates[k];
  }
  if (this->weak) {
    return this->weakCandidates[k];
  }

  return this->candidates[k];
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
  std::vector<InferStep> inner;
  llvm::ArrayRef<const Type *> ps = pattern->params();
  bool isList =
      pattern->hasRest() && (ps.back()->isListParam() || ArgListChecker::spreadsLists(ps.back()));
  size_t fixed = isList ? ps.size() - 1 : ps.size();
  for (size_t i = 0; i < fixed && i < actual->params().size(); ++i) {
    inner.push_back(InferPair{ps[i], actual->params()[i]});
  }
  if (isList && actual->params().size() - (actual->hasRest() ? 1 : 0) >= fixed) {
    inner.push_back(InferPair{ps.back(), Inference::paramsAsList(actual, fixed)});
  }
  std::optional<size_t> returned = this->paramIndex(pattern->ret());
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
  if (pattern->isRecord() && actual->isObject() && !actual->isClass() && !actual->isTuple()) {
    std::vector<InferStep> inner;
    for (const Field &f : actual->fields()) {
      inner.push_back(InferPair{pattern->valueType(), f.type});
    }
    return inner;
  }
  if (!actual->isDict() || actual->dictKind() != pattern->dictKind()) {
    return {};
  }

  return {InferPair{pattern->keyType(), actual->keyType()},
          InferPair{pattern->valueType(), actual->valueType()}};
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
  auto [_, isFirstVisit] = this->visited.insert({pattern, actual});
  if (!isFirstVisit) {
    return {};
  }
  if (ArgListChecker::spreadsLists(pattern) && actual->isTuple()) {
    return {InferList{pattern, actual, nullptr}};
  }
  const ClassDecl *cls = pattern->classDecl();
  if (cls != nullptr && !pattern->typeArgs().empty()) {
    return Inference::inferClassArgs(pattern, cls, actual);
  }
  const AliasOrigin *origin = this->origins.findAliasOrigin(pattern);
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

std::vector<InferStep> Inference::inferClassArgs(const Type *pattern, const ClassDecl *cls,
                                                 const Type *actual) {
  const ClassDecl *generic = cls->generic() != nullptr ? cls->generic() : cls;
  const ClassDecl *found = GenericClassChecker::genericInstanceOf(actual, generic);
  if (found == nullptr) {
    return {};
  }

  return Inference::pairArgs(pattern->typeArgs(), found->typeArgs);
}

std::optional<std::vector<InferStep>> Inference::inferAliasArgs(const AliasOrigin *origin,
                                                                const Type *actual) {
  std::vector<const Type *> queue{actual};
  for (size_t i = 0; i < queue.size(); ++i) {
    const Type *t = queue[i];
    const AliasOrigin *other = this->origins.findAliasOrigin(t);
    if (other != nullptr && other->alias == origin->alias) {
      return Inference::pairArgs(origin->args, other->args);
    }
    queue.insert(queue.end(), t->bases().begin(), t->bases().end());
  }

  return std::nullopt;
}

std::vector<InferStep> Inference::pairArgs(llvm::ArrayRef<const Type *> want,
                                           llvm::ArrayRef<const Type *> have) {
  std::vector<InferStep> inner;
  for (size_t k = 0; k < want.size() && k < have.size(); ++k) {
    inner.push_back(InferPair{want[k], have[k]});
  }

  return inner;
}

std::vector<InferStep> Inference::inferFields(const Type *pattern, const Type *actual) {
  std::vector<InferStep> inner;
  for (const Field &f : pattern->fields()) {
    const Field *a = actual->field(f.name);
    if (a != nullptr) {
      inner.push_back(InferPair{f.type, a->type});
    }
  }

  return inner;
}

std::vector<InferStep> Inference::inferUnion(const Type *pattern, const Type *actual) {
  std::vector<const Type *> open;
  std::vector<const Type *> fixed;
  for (const Type *m : pattern->members()) {
    if (GenericCheckHelpers::mentionsAnyOf(m, this->params)) {
      open.push_back(m);
    } else {
      fixed.push_back(m);
    }
  }
  if (open.size() > 1) {
    return Inference::inferGenericMember(open, actual);
  }
  if (open.empty()) {
    return {};
  }
  std::vector<const Type *> rest = Inference::membersOutside(actual->nonOptional(), fixed);
  if (rest.empty()) {
    return {};
  }

  return {InferPair{open.front(), Type::unionOf(rest)}};
}

std::vector<InferStep> Inference::inferGenericMember(llvm::ArrayRef<const Type *> open,
                                                     const Type *actual) {
  const Type *value = actual->nonOptional();
  for (const Type *m : open) {
    const ClassDecl *cls = m->classDecl();
    if (cls == nullptr || m->typeArgs().empty()) {
      continue;
    }
    const ClassDecl *generic = cls->generic() != nullptr ? cls->generic() : cls;
    if (GenericClassChecker::genericInstanceOf(value, generic) != nullptr) {
      return {InferPair{m, value}};
    }
  }

  return {};
}

std::vector<const Type *> Inference::membersOutside(const Type *value,
                                                    llvm::ArrayRef<const Type *> fixed) {
  llvm::ArrayRef<const Type *> members =
      value->isUnion() ? value->members() : llvm::ArrayRef<const Type *>(value);
  std::vector<const Type *> rest;
  for (const Type *m : members) {
    if (!llvm::is_contained(fixed, m)) {
      rest.push_back(m);
    }
  }

  return rest;
}

std::vector<InferStep> Inference::inferHeld(const Type *pattern, const Type *actual) {
  if (auto at = llvm::find(this->params, pattern); at != this->params.end() && actual->isVoid()) {
    (this->weak ? this->weakCandidates : this->candidates)[size_t(at - this->params.begin())]
        .push_back(actual);
    return {};
  }

  return {InferPair{pattern, actual}};
}

std::vector<InferStep> Inference::inferList(const Type *pattern, const Type *actual,
                                            const std::vector<const Type *> *bound) {
  std::vector<InferStep> inner;
  llvm::ArrayRef<Field> parts = pattern->fields();
  size_t count = actual->fields().size();
  std::vector<int> lengths(parts.size(), 1);
  int unknown = -1;
  size_t known = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    const Type *part = parts[i].type;
    if (!part->isListParam()) {
      ++known;
      continue;
    }
    size_t k = size_t(llvm::find(this->params, part) - this->params.begin());
    const Type *value = bound && k < bound->size() ? (*bound)[k] : nullptr;
    if (value && value->isTuple() && !value->tupleRest() &&
        value->tupleMin() == value->fields().size()) {
      lengths[i] = int(value->fields().size());
      known += value->fields().size();
    } else if (unknown >= 0) {
      if (!bound) {
        this->deferred.push_back({pattern, actual});
      }
      return inner;
    } else {
      unknown = int(i);
    }
  }
  if (known > count || (actual->tupleRest() && unknown != int(parts.size()) - 1)) {
    return inner;
  }
  if (unknown >= 0) {
    lengths[size_t(unknown)] = int(count - known);
  } else if (known != count) {
    return inner;
  }
  size_t at = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    size_t length = size_t(lengths[i]);
    if (parts[i].type->isListParam()) {
      inner.push_back(InferPair{
          parts[i].type, Inference::listSlice(actual, at, at + length, i + 1 == parts.size())});
    } else if (at < count) {
      inner.push_back(InferPair{parts[i].type, actual->fields()[at].type});
    }
    at += length;
  }

  return inner;
}

void Inference::from(const Type *pattern, const Type *actual) {
  this->runInference(InferPair{pattern, actual});
}

void Inference::fromHeld(const Type *pattern, const Type *actual) {
  this->runInference(InferHeld{pattern, actual});
}

void Inference::matchList(const Type *pattern, const Type *actual,
                          const std::vector<const Type *> *bound) {
  this->runInference(InferList{pattern, actual, bound});
}

void Inference::runInference(InferStep first) {
  std::vector<InferStep> steps = {first};
  while (!steps.empty()) {
    InferStep step = steps.back();
    steps.pop_back();
    std::vector<InferStep> inner = this->applyInferStep(step);
    steps.insert(steps.end(), inner.rbegin(), inner.rend());
  }
}

std::vector<InferStep> Inference::applyInferStep(const InferStep &step) {
  switch (step.kind()) {
    case InferStepKind::PAIR:
      return this->inferPair(step.as<InferPair>()->pattern, step.as<InferPair>()->actual);
    case InferStepKind::HELD:
      return this->inferHeld(step.as<InferHeld>()->pattern, step.as<InferHeld>()->actual);
    case InferStepKind::LIST: {
      const InferList *list = step.as<InferList>();
      return this->inferList(list->pattern, list->actual, list->bound);
    }
    case InferStepKind::CANDIDATE: {
      const AddCandidate *candidate = step.as<AddCandidate>();
      this->candidates[candidate->index].push_back(candidate->type);
      return {};
    }
  }
  llvm_unreachable("unknown inference step");
}

void Inference::retry(const std::vector<const Type *> &bound) {
  std::vector<std::pair<const Type *, const Type *>> waiting = std::move(this->deferred);
  this->deferred.clear();
  for (auto [pattern, actual] : waiting) {
    this->matchList(pattern, actual, &bound);
  }
}

}
