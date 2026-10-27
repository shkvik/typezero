#include "compiler/check/generics/inference/inference.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params) {
  if (params.empty() || !mentionsParam(type)) {
    return false;
  }
  llvm::SmallPtrSet<const Type *, 4> seen;

  return mentionsAnyOf(type, params, seen);
}

const Type *paramsAsList(const Type *fn, size_t from) {
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

const Type *listSlice(const Type *list, size_t begin, size_t end, bool last) {
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

Inference::Inference(llvm::ArrayRef<const Type *> params, const Origins &origins,
                     std::function<const Type *(const Type *)> elementOf)
    : params(params), candidates(params.size()), weakCandidates(params.size()),
      literalCandidates(params.size()), undefinedSeen(params.size(), false), origins(origins),
      elementOf(std::move(elementOf)) {}

void Inference::from(const Type *pattern, const Type *actual) {
  if (actual->isError() || actual->isVoid()) {
    return;
  }
  if (auto at = llvm::find(params, pattern); at != params.end()) {
    size_t k = size_t(at - params.begin());
    if (actual->isUndefined()) {
      undefinedSeen[k] = true;
      return;
    }
    const Type *element = pattern->iterated();
    const Type *candidate = actual;
    if (element && weak && actual->isArray() && !mentionsParam(element)) {
      candidate = Type::array(element);
    }
    (intLiteral ? literalCandidates : weak ? weakCandidates : candidates)[k].push_back(candidate);
    if (element && elementOf) {
      if (const Type *given = elementOf(candidate)) {
        from(element, given);
      }
    }
    return;
  }
  if (!mentionsAnyOf(pattern, params)) {
    return;
  }
  switch (pattern->kind()) {
  case Type::Kind::Optional:
    if (!actual->isUndefined()) {
      from(pattern->inner(), actual->nonOptional());
    }
    return;
  case Type::Kind::Array:
    if (actual->isArray()) {
      from(pattern->inner(), actual->inner());
    }
    return;
  case Type::Kind::Result:
    if (actual->isResult()) {
      fromHeld(pattern->okType(), actual->okType());
      from(pattern->errType(), actual->errType());
    }
    return;
  case Type::Kind::Function: {
    if (!actual->isFunction()) {
      return;
    }
    llvm::ArrayRef<const Type *> ps = pattern->params();
    bool list = pattern->hasRest() && (ps.back()->isListParam() || spreadsLists(ps.back()));
    size_t fixed = list ? ps.size() - 1 : ps.size();
    for (size_t i = 0; i < fixed && i < actual->params().size(); ++i) {
      from(ps[i], actual->params()[i]);
    }
    if (list && actual->params().size() - (actual->hasRest() ? 1 : 0) >= fixed) {
      from(ps.back(), paramsAsList(actual, fixed));
    }
    if (auto at = llvm::find(params, pattern->ret());
        at != params.end() && actual->ret()->isVoid()) {
      candidates[size_t(at - params.begin())].push_back(actual->ret());
    } else if (!pattern->ret()->isVoid()) {
      from(pattern->ret(), actual->ret());
    }
    return;
  }
  case Type::Kind::Iterator:
    if (!actual->isIterator()) {
      return;
    }
    from(pattern->yielded(), actual->yielded());
    from(pattern->returned(), actual->returned());
    from(pattern->sent(), actual->sent());
    return;
  case Type::Kind::Dict:
    if (pattern->isRecord() && actual->isObject() && !actual->isClass() && !actual->isTuple()) {
      for (const Field &f : actual->fields()) {
        from(pattern->valueType(), f.type);
      }
      return;
    }
    if (!actual->isDict() || actual->dictKind() != pattern->dictKind()) {
      return;
    }
    from(pattern->keyType(), actual->keyType());
    from(pattern->valueType(), actual->valueType());
    return;
  case Type::Kind::Promise:
    if (actual->isPromise()) {
      fromHeld(pattern->promised(), actual->promised());
    }
    return;
  case Type::Kind::ClassRef:
    if (pattern->isConstructor() && actual->isClassRef()) {
      from(pattern->constructed(),
           actual->isConstructor() ? actual->constructed() : actual->classRef()->type);
    }
    return;
  case Type::Kind::Object: {
    if (pattern->isAsyncIterator()) {
      if (!actual->isAsyncIterator() && asyncOf) {
        if (const Type *given = asyncOf(actual)) {
          actual = given;
        }
      }
      if (actual->isAsyncIterator()) {
        from(pattern->yielded(), actual->yielded());
        from(pattern->returned(), actual->returned());
        from(pattern->sent(), actual->sent());
      }
      return;
    }
    if (!actual->isObject() || !visited.insert({pattern, actual}).second) {
      return;
    }
    if (spreadsLists(pattern) && actual->isTuple()) {
      matchList(pattern, actual, nullptr);
      return;
    }
    if (const ClassDecl *cls = pattern->classDecl(); cls && !pattern->typeArgs().empty()) {
      const ClassDecl *generic = cls->generic ? cls->generic : cls;
      if (const ClassDecl *found = genericInstanceOf(actual, generic)) {
        for (size_t k = 0; k < pattern->typeArgs().size() && k < found->typeArgs.size(); ++k) {
          from(pattern->typeArgs()[k], found->typeArgs[k]);
        }
      }
      return;
    }
    if (auto origin = origins.find(pattern); origin != origins.end()) {
      std::vector<const Type *> queue{actual};
      for (size_t i = 0; i < queue.size(); ++i) {
        const Type *t = queue[i];
        auto other = origins.find(t);
        if (other != origins.end() && other->second.first == origin->second.first) {
          const std::vector<const Type *> &want = origin->second.second,
                                          &have = other->second.second;
          for (size_t k = 0; k < want.size() && k < have.size(); ++k) {
            from(want[k], have[k]);
          }
          return;
        }
        queue.insert(queue.end(), t->bases().begin(), t->bases().end());
      }
    }
    for (const Field &f : pattern->fields()) {
      if (const Field *a = actual->field(f.name)) {
        from(f.type, a->type);
      }
    }
    return;
  }
  case Type::Kind::Union: {
    std::vector<const Type *> open, fixed;
    for (const Type *m : pattern->members()) {
      (mentionsAnyOf(m, params) ? open : fixed).push_back(m);
    }
    if (open.size() != 1) {
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
      from(open.front(), Type::unionOf(rest));
    }
    return;
  }
  default:
    return;
  }
}

void Inference::fromHeld(const Type *pattern, const Type *actual) {
  if (auto at = llvm::find(params, pattern); at != params.end() && actual->isVoid()) {
    (weak ? weakCandidates : candidates)[size_t(at - params.begin())].push_back(actual);
    return;
  }
  from(pattern, actual);
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
    size_t k = size_t(llvm::find(params, part) - params.begin());
    const Type *value = bound && k < bound->size() ? (*bound)[k] : nullptr;
    if (value && value->isTuple() && !value->tupleRest() &&
        value->tupleMin() == value->fields().size()) {
      lengths[i] = int(value->fields().size());
      known += value->fields().size();
    } else if (unknown >= 0) {
      if (!bound) {
        deferred.push_back({pattern, actual});
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
      from(parts[i].type, listSlice(actual, at, at + length, i + 1 == parts.size()));
    } else if (at < count) {
      from(parts[i].type, actual->fields()[at].type);
    }
    at += length;
  }
}

void Inference::retry(const std::vector<const Type *> &bound) {
  std::vector<std::pair<const Type *, const Type *>> waiting = std::move(deferred);
  deferred.clear();
  for (auto [pattern, actual] : waiting) {
    matchList(pattern, actual, &bound);
  }
}

}
