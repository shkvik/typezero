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

void Inference::from(const Type *pattern, const Type *actual) {
  if (actual->isError()) {
    return;
  }
  if (actual->isVoid()) {
    if (auto at = llvm::find(this->params, pattern); at != this->params.end()) {
      this->candidates[size_t(at - this->params.begin())].push_back(actual);
    }
    return;
  }
  if (auto at = llvm::find(this->params, pattern); at != this->params.end()) {
    size_t k = size_t(at - this->params.begin());
    if (actual->isUndefined()) {
      this->undefinedSeen[k] = true;
      return;
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
        this->from(element, given);
      }
    }
    return;
  }
  if (!GenericCheckHelpers::mentionsAnyOf(pattern, this->params)) {
    return;
  }
  switch (pattern->kind()) {
  case Type::Kind::Optional:
    if (!actual->isUndefined()) {
      this->from(pattern->inner(), actual->nonOptional());
    }
    return;
  case Type::Kind::Array:
    if (actual->isArray()) {
      this->from(pattern->inner(), actual->inner());
    }
    return;
  case Type::Kind::Result:
    if (actual->isResult()) {
      this->fromHeld(pattern->okType(), actual->okType());
      this->from(pattern->errType(), actual->errType());
    }
    return;
  case Type::Kind::Function: {
    if (!actual->isFunction()) {
      return;
    }
    llvm::ArrayRef<const Type *> ps = pattern->params();
    bool list =
        pattern->hasRest() && (ps.back()->isListParam() || ArgListChecker::spreadsLists(ps.back()));
    size_t fixed = list ? ps.size() - 1 : ps.size();
    for (size_t i = 0; i < fixed && i < actual->params().size(); ++i) {
      this->from(ps[i], actual->params()[i]);
    }
    if (list && actual->params().size() - (actual->hasRest() ? 1 : 0) >= fixed) {
      this->from(ps.back(), Inference::paramsAsList(actual, fixed));
    }
    if (auto at = llvm::find(this->params, pattern->ret());
        at != this->params.end() && actual->ret()->isVoid()) {
      this->candidates[size_t(at - this->params.begin())].push_back(actual->ret());
    } else if (!pattern->ret()->isVoid()) {
      this->from(pattern->ret(), actual->ret());
    }
    return;
  }
  case Type::Kind::Iterator:
    if (!actual->isIterator()) {
      return;
    }
    this->from(pattern->yielded(), actual->yielded());
    this->from(pattern->returned(), actual->returned());
    this->from(pattern->sent(), actual->sent());
    return;
  case Type::Kind::Dict:
    if (pattern->isRecord() && actual->isObject() && !actual->isClass() && !actual->isTuple()) {
      for (const Field &f : actual->fields()) {
        this->from(pattern->valueType(), f.type);
      }
      return;
    }
    if (!actual->isDict() || actual->dictKind() != pattern->dictKind()) {
      return;
    }
    this->from(pattern->keyType(), actual->keyType());
    this->from(pattern->valueType(), actual->valueType());
    return;
  case Type::Kind::Promise:
    if (actual->isPromise()) {
      this->fromHeld(pattern->promised(), actual->promised());
    }
    return;
  case Type::Kind::ClassRef:
    if (pattern->isConstructor() && actual->isClassRef()) {
      this->from(pattern->constructed(),
                 actual->isConstructor() ? actual->constructed() : actual->classRef()->type);
    }
    return;
  case Type::Kind::Object: {
    if (pattern->isAsyncIterator()) {
      if (!actual->isAsyncIterator() && this->asyncOf) {
        if (const Type *given = this->asyncOf(actual)) {
          actual = given;
        }
      }
      if (actual->isAsyncIterator()) {
        this->from(pattern->yielded(), actual->yielded());
        this->from(pattern->returned(), actual->returned());
        this->from(pattern->sent(), actual->sent());
      }
      return;
    }
    if (!actual->isObject() || !this->visited.insert({pattern, actual}).second) {
      return;
    }
    if (ArgListChecker::spreadsLists(pattern) && actual->isTuple()) {
      this->matchList(pattern, actual, nullptr);
      return;
    }
    if (const ClassDecl *cls = pattern->classDecl(); cls && !pattern->typeArgs().empty()) {
      const ClassDecl *generic = cls->generic() ? cls->generic() : cls;
      if (const ClassDecl *found = GenericClassChecker::genericInstanceOf(actual, generic)) {
        for (size_t k = 0; k < pattern->typeArgs().size() && k < found->typeArgs.size(); ++k) {
          this->from(pattern->typeArgs()[k], found->typeArgs[k]);
        }
      }
      return;
    }
    if (const AliasOrigin *origin = this->origins.findAliasOrigin(pattern)) {
      std::vector<const Type *> queue{actual};
      for (size_t i = 0; i < queue.size(); ++i) {
        const Type *t = queue[i];
        const AliasOrigin *other = this->origins.findAliasOrigin(t);
        if (other && other->alias == origin->alias) {
          const std::vector<const Type *> &want = origin->args, &have = other->args;
          for (size_t k = 0; k < want.size() && k < have.size(); ++k) {
            this->from(want[k], have[k]);
          }
          return;
        }
        queue.insert(queue.end(), t->bases().begin(), t->bases().end());
      }
    }
    for (const Field &f : pattern->fields()) {
      if (const Field *a = actual->field(f.name)) {
        this->from(f.type, a->type);
      }
    }
    return;
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
          this->from(m, value);
          return;
        }
      }
      return;
    }
    if (open.empty()) {
      return;
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
      this->from(open.front(), Type::unionOf(rest));
    }
    return;
  }
  default:
    return;
  }
}

void Inference::fromHeld(const Type *pattern, const Type *actual) {
  if (auto at = llvm::find(this->params, pattern); at != this->params.end() && actual->isVoid()) {
    (this->weak ? this->weakCandidates : this->candidates)[size_t(at - this->params.begin())]
        .push_back(actual);
    return;
  }
  this->from(pattern, actual);
}

void Inference::matchList(const Type *pattern, const Type *actual,
                          const std::vector<const Type *> *bound) {
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
      return;
    } else {
      unknown = int(i);
    }
  }
  if (known > count || (actual->tupleRest() && unknown != int(parts.size()) - 1)) {
    return;
  }
  if (unknown >= 0) {
    lengths[size_t(unknown)] = int(count - known);
  } else if (known != count) {
    return;
  }
  size_t at = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    size_t length = size_t(lengths[i]);
    if (parts[i].type->isListParam()) {
      this->from(parts[i].type,
                 Inference::listSlice(actual, at, at + length, i + 1 == parts.size()));
    } else if (at < count) {
      this->from(parts[i].type, actual->fields()[at].type);
    }
    at += length;
  }
}

void Inference::retry(const std::vector<const Type *> &bound) {
  std::vector<std::pair<const Type *, const Type *>> waiting = std::move(this->deferred);
  this->deferred.clear();
  for (auto [pattern, actual] : waiting) {
    this->matchList(pattern, actual, &bound);
  }
}

}
