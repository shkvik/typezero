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
  std::vector<InferStep> inner;
  if (actual->isError()) {
    return inner;
  }
  if (actual->isVoid()) {
    if (auto at = llvm::find(this->params, pattern); at != this->params.end()) {
      this->candidates[size_t(at - this->params.begin())].push_back(actual);
    }
    return inner;
  }
  if (auto at = llvm::find(this->params, pattern); at != this->params.end()) {
    size_t k = size_t(at - this->params.begin());
    if (actual->isUndefined()) {
      this->undefinedSeen[k] = true;
      return inner;
    }
    const Type *element = pattern->iterated();
    const Type *candidate = actual;
    if (element && this->weak && actual->isArray() && !TypeQueries::mentionsParam(element)) {
      candidate = Type::array(element);
    }
    (this->intLiteral ? this->literalCandidates
     : this->weak     ? this->weakCandidates
                      : this->candidates)[k]
        .push_back(candidate);
    if (element && this->elementOf) {
      if (const Type *given = this->elementOf(candidate)) {
        inner.push_back(InferPair{element, given});
      }
    }
    return inner;
  }
  if (!GenericCheckHelpers::mentionsAnyOf(pattern, this->params)) {
    return inner;
  }
  switch (pattern->kind()) {
  case Type::Kind::Optional:
    if (!actual->isUndefined()) {
      inner.push_back(InferPair{pattern->inner(), actual->nonOptional()});
    }
    return inner;
  case Type::Kind::Array:
    if (actual->isArray()) {
      inner.push_back(InferPair{pattern->inner(), actual->inner()});
    }
    return inner;
  case Type::Kind::Result:
    if (actual->isResult()) {
      inner.push_back(InferHeld{pattern->okType(), actual->okType()});
      inner.push_back(InferPair{pattern->errType(), actual->errType()});
    }
    return inner;
  case Type::Kind::Function: {
    if (!actual->isFunction()) {
      return inner;
    }
    llvm::ArrayRef<const Type *> ps = pattern->params();
    bool list =
        pattern->hasRest() && (ps.back()->isListParam() || ArgListChecker::spreadsLists(ps.back()));
    size_t fixed = list ? ps.size() - 1 : ps.size();
    for (size_t i = 0; i < fixed && i < actual->params().size(); ++i) {
      inner.push_back(InferPair{ps[i], actual->params()[i]});
    }
    if (list && actual->params().size() - (actual->hasRest() ? 1 : 0) >= fixed) {
      inner.push_back(InferPair{ps.back(), Inference::paramsAsList(actual, fixed)});
    }
    if (auto at = llvm::find(this->params, pattern->ret());
        at != this->params.end() && actual->ret()->isVoid()) {
      inner.push_back(AddCandidate{size_t(at - this->params.begin()), actual->ret()});
    } else if (!pattern->ret()->isVoid()) {
      inner.push_back(InferPair{pattern->ret(), actual->ret()});
    }
    return inner;
  }
  case Type::Kind::Iterator:
    if (!actual->isIterator()) {
      return inner;
    }
    inner.push_back(InferPair{pattern->yielded(), actual->yielded()});
    inner.push_back(InferPair{pattern->returned(), actual->returned()});
    inner.push_back(InferPair{pattern->sent(), actual->sent()});
    return inner;
  case Type::Kind::Dict:
    if (pattern->isRecord() && actual->isObject() && !actual->isClass() && !actual->isTuple()) {
      for (const Field &f : actual->fields()) {
        inner.push_back(InferPair{pattern->valueType(), f.type});
      }
      return inner;
    }
    if (!actual->isDict() || actual->dictKind() != pattern->dictKind()) {
      return inner;
    }
    inner.push_back(InferPair{pattern->keyType(), actual->keyType()});
    inner.push_back(InferPair{pattern->valueType(), actual->valueType()});
    return inner;
  case Type::Kind::Promise:
    if (actual->isPromise()) {
      inner.push_back(InferHeld{pattern->promised(), actual->promised()});
    }
    return inner;
  case Type::Kind::ClassRef:
    if (pattern->isConstructor() && actual->isClassRef()) {
      inner.push_back(InferPair{pattern->constructed(), actual->isConstructor()
                                                            ? actual->constructed()
                                                            : actual->classRef()->type});
    }
    return inner;
  case Type::Kind::Object: {
    if (pattern->isAsyncIterator()) {
      if (!actual->isAsyncIterator() && this->asyncOf) {
        if (const Type *given = this->asyncOf(actual)) {
          actual = given;
        }
      }
      if (actual->isAsyncIterator()) {
        inner.push_back(InferPair{pattern->yielded(), actual->yielded()});
        inner.push_back(InferPair{pattern->returned(), actual->returned()});
        inner.push_back(InferPair{pattern->sent(), actual->sent()});
      }
      return inner;
    }
    if (!actual->isObject() || !this->visited.insert({pattern, actual}).second) {
      return inner;
    }
    if (ArgListChecker::spreadsLists(pattern) && actual->isTuple()) {
      inner.push_back(InferList{pattern, actual, nullptr});
      return inner;
    }
    if (const ClassDecl *cls = pattern->classDecl(); cls && !pattern->typeArgs().empty()) {
      const ClassDecl *generic = cls->generic() ? cls->generic() : cls;
      if (const ClassDecl *found = GenericClassChecker::genericInstanceOf(actual, generic)) {
        for (size_t k = 0; k < pattern->typeArgs().size() && k < found->typeArgs.size(); ++k) {
          inner.push_back(InferPair{pattern->typeArgs()[k], found->typeArgs[k]});
        }
      }
      return inner;
    }
    if (const AliasOrigin *origin = this->origins.findAliasOrigin(pattern)) {
      std::vector<const Type *> queue{actual};
      for (size_t i = 0; i < queue.size(); ++i) {
        const Type *t = queue[i];
        const AliasOrigin *other = this->origins.findAliasOrigin(t);
        if (other && other->alias == origin->alias) {
          const std::vector<const Type *> &want = origin->args, &have = other->args;
          for (size_t k = 0; k < want.size() && k < have.size(); ++k) {
            inner.push_back(InferPair{want[k], have[k]});
          }
          return inner;
        }
        queue.insert(queue.end(), t->bases().begin(), t->bases().end());
      }
    }
    for (const Field &f : pattern->fields()) {
      if (const Field *a = actual->field(f.name)) {
        inner.push_back(InferPair{f.type, a->type});
      }
    }
    return inner;
  }
  case Type::Kind::Union: {
    std::vector<const Type *> open, fixed;
    for (const Type *m : pattern->members()) {
      (GenericCheckHelpers::mentionsAnyOf(m, this->params) ? open : fixed).push_back(m);
    }
    if (open.size() > 1) {
      const Type *value = actual->nonOptional();
      for (const Type *m : open) {
        const ClassDecl *cls = m->classDecl();
        if (cls && !m->typeArgs().empty() &&
            GenericClassChecker::genericInstanceOf(value, cls->generic() ? cls->generic() : cls)) {
          inner.push_back(InferPair{m, value});
          return inner;
        }
      }
      return inner;
    }
    if (open.empty()) {
      return inner;
    }
    std::vector<const Type *> rest;
    const Type *value = actual->nonOptional();
    for (const Type *m :
         value->isUnion() ? value->members() : llvm::ArrayRef<const Type *>(value)) {
      if (!llvm::is_contained(fixed, m)) {
        rest.push_back(m);
      }
    }
    if (!rest.empty()) {
      inner.push_back(InferPair{open.front(), Type::unionOf(rest)});
    }
    return inner;
  }
  default:
    return inner;
  }
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
