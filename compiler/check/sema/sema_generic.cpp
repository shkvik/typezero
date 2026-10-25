// Generics: generic functions and type aliases (docs/generics.md).
//
// The body of a generic function is checked once, as in TypeScript, with its
// type parameters as opaque types (Type::Kind::Param). A value of type T can
// be assigned, passed, returned, put in arrays, compared with `==`, printed,
// tested with `typeof` and turned into a string; `T extends number` adds what
// a number can do, and arithmetic on it gives a number of the constraint's
// type, as in TypeScript. Errors there are reported in the body.
//
// A call infers the type arguments: from the arguments, then from the type
// expected where the call stands, then from the literals passed (which take
// the types found so far: `pair(1, 2.5)` makes T a double, and 1 a double),
// then from the arrows (whose parameters need the types found so far), then
// from the defaults. It calls
// an instance, a copy of the function made for those types: the body parsed
// again (FunctionDecl::source) and checked with them, by every rule of the
// language. What our rules reject and TypeScript accepts, such as turning an
// object into a string or `<` on objects, shows up there, and is reported at
// the call that needs the instance. The instance is what CodeGen emits, as an
// ordinary function, so generic code is as fast as code written for its
// types. Types that look the same in memory share an instance: all objects
// are a pointer released through its descriptor, all functions a code and an
// environment (see representation).
//
// The generic check decides how a value of a type parameter converts: to
// double for `x * 2` with `T extends number`, to a string next to one. An
// instance makes the same conversion of the same expression whatever its
// type (noteForced), so the meaning of the body does not depend on T. In an
// instance, `typeof x == "string"` with x of a known type is a constant, and
// the branch that cannot run is dropped without being checked: it was written
// for other types.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

namespace {

// How many instances of one generic function a chain of instances that need
// each other may hold before it counts as endless: `f(x: T)` calling `g([x])`
// and g calling f needs f<int>, f<int[]>, f<int[][]>...
constexpr unsigned kMaxInstanceChain = 16;
// How deep instances of generic aliases may nest, each needed by the one
// that resolves it, as in TypeScript: an alias that refers to itself with
// other arguments, or a conditional type whose branch does. A conditional
// type that ends in another instance, a tail call, goes on in a loop
// instead, up to kMaxTailCalls times (TypeScript's limits too).
constexpr unsigned kMaxAliasDepth = 50;
constexpr unsigned kMaxTailCalls = 1000;

bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params,
                   llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    // `T[K]` and the other computations, by what they are made of.
    if (type->typeOp() != TypeOp::None)
      return llvm::any_of(type->operands(), [&](const Type *t) { return t && mentionsAnyOf(t, params, seen); });
    return llvm::is_contained(params, type);
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return mentionsAnyOf(type->inner(), params, seen);
  case Type::Kind::Result:
    return mentionsAnyOf(type->okType(), params, seen) || mentionsAnyOf(type->errType(), params, seen);
  case Type::Kind::Function:
    return mentionsAnyOf(type->ret(), params, seen) ||
           llvm::any_of(type->params(), [&](const Type *p) { return mentionsAnyOf(p, params, seen); });
  case Type::Kind::Iterator:
    return mentionsAnyOf(type->yielded(), params, seen) ||
           mentionsAnyOf(type->returned(), params, seen) ||
           mentionsAnyOf(type->sent(), params, seen);
  case Type::Kind::Dict:
    return mentionsAnyOf(type->keyType(), params, seen) ||
           mentionsAnyOf(type->valueType(), params, seen);
  case Type::Kind::ClassRef: // `Constructor<T>`
    return type->isConstructor() && mentionsAnyOf(type->constructed(), params, seen);
  case Type::Kind::Object:
    // An async iterator, as an iterator.
    if (type->isAsyncIterator())
      return mentionsAnyOf(type->yielded(), params, seen) ||
             mentionsAnyOf(type->returned(), params, seen) ||
             mentionsAnyOf(type->sent(), params, seen);
    // A class made from a generic one: by its type arguments.
    if (!type->typeArgs().empty())
      return llvm::any_of(type->typeArgs(), [&](const Type *t) { return mentionsAnyOf(t, params, seen); });
    if (!seen.insert(type).second)
      return false;
    if (type->tupleRest() && mentionsAnyOf(type->tupleRest(), params, seen))
      return true;
    return llvm::any_of(type->fields(), [&](const Field &f) { return mentionsAnyOf(f.type, params, seen); });
  case Type::Kind::Union:
    return llvm::any_of(type->members(),
                        [&](const Type *m) { return mentionsAnyOf(m, params, seen); });
  default:
    return false;
  }
}

// Whether `type` mentions one of `params`.
bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params) {
  if (params.empty() || !mentionsParam(type))
    return false;
  llvm::SmallPtrSet<const Type *, 4> seen;
  return mentionsAnyOf(type, params, seen);
}

// Whether `type` holds an object type that mentions one of `params`: a
// class made from a generic one, an interface made from a generic one, a
// literal type. Objects of such types for T = User and for T = Post are
// told apart by their descriptors, unlike arrays of them.
bool mentionsInObject(const Type *type, llvm::ArrayRef<const Type *> params,
                      llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  // `keyof T`, `T[K]`: of the very type T.
  case Type::Kind::Param:
    return type->typeOp() != TypeOp::None && mentionsAnyOf(type, params);
  case Type::Kind::Optional:
  case Type::Kind::Array:
    return mentionsInObject(type->inner(), params, seen);
  case Type::Kind::Result:
    return mentionsInObject(type->okType(), params, seen) || mentionsInObject(type->errType(), params, seen);
  case Type::Kind::Function:
    return mentionsInObject(type->ret(), params, seen) ||
           llvm::any_of(type->params(), [&](const Type *p) { return mentionsInObject(p, params, seen); });
  case Type::Kind::Iterator:
    return mentionsInObject(type->yielded(), params, seen) || mentionsInObject(type->returned(), params, seen);
  case Type::Kind::Dict:
    return mentionsInObject(type->keyType(), params, seen) || mentionsInObject(type->valueType(), params, seen);
  case Type::Kind::Union:
    return llvm::any_of(type->members(), [&](const Type *m) { return mentionsInObject(m, params, seen); });
  case Type::Kind::Object:
    return !seen.contains(type) && mentionsAnyOf(type, params);
  default:
    return false;
  }
}

// Whether one of `params` occurs inside an array, a function or an object in
// `type`: a call of a generic function from its own body with such a type
// argument needs a bigger instance each time. (`T | undefined` does not grow:
// it is the same type again one level down.)
bool grows(const Type *type, llvm::ArrayRef<const Type *> params, bool inside) {
  switch (type->kind()) {
  case Type::Kind::Param:
    return inside && llvm::is_contained(params, type);
  case Type::Kind::Optional:
    return grows(type->inner(), params, inside);
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return grows(type->inner(), params, true);
  // A Result is a value, but `Result<T, never>` is another type than T, one
  // level bigger.
  case Type::Kind::Result:
    return grows(type->okType(), params, true) || grows(type->errType(), params, true);
  case Type::Kind::Function:
    return grows(type->ret(), params, true) ||
           llvm::any_of(type->params(), [&](const Type *p) { return grows(p, params, true); });
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return mentionsAnyOf(type, params);
  default:
    return false;
  }
}

// The class of an object literal with methods that `type` holds, if any. A
// class declared by name is the same in every instance; the class of such a
// literal belongs to the copy of the body that has it.
const ClassDecl *literalClassIn(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return literalClassIn(type->inner(), seen);
  case Type::Kind::Result:
    if (const ClassDecl *cls = literalClassIn(type->okType(), seen))
      return cls;
    return literalClassIn(type->errType(), seen);
  case Type::Kind::Function:
    for (const Type *p : type->params())
      if (const ClassDecl *cls = literalClassIn(p, seen))
        return cls;
    return literalClassIn(type->ret(), seen);
  case Type::Kind::Iterator:
    for (const Type *t : {type->yielded(), type->returned(), type->sent()})
      if (const ClassDecl *cls = literalClassIn(t, seen))
        return cls;
    return nullptr;
  case Type::Kind::Dict:
    if (const ClassDecl *cls = literalClassIn(type->keyType(), seen))
      return cls;
    return literalClassIn(type->valueType(), seen);
  case Type::Kind::Object:
    if (!seen.insert(type).second)
      return nullptr;
    if (const ClassDecl *cls = type->classDecl())
      return cls->literal ? cls : nullptr;
    for (const Field &f : type->fields())
      if (const ClassDecl *cls = literalClassIn(f.type, seen))
        return cls;
    return nullptr;
  default:
    return nullptr;
  }
}

// "int, string"
std::string typeList(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types)
    text += (text.empty() ? "" : ", ") + t->name().str();
  return text;
}

// "T = int, U = string"
std::string bindingList(llvm::ArrayRef<const Type *> params, llvm::ArrayRef<const Type *> args) {
  std::string text;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i)
    text += (text.empty() ? "" : ", ") + params[i]->name().str() + " = " + args[i]->name().str();
  return text;
}

// `T extends string | int`: "string | int". `number`, as written, rather
// than the double it is.
std::string constraintSpelling(const Type *param, const TypeParamDecl *decl = nullptr) {
  if (decl && decl->constraint && decl->constraint->kind == TypeRef::Kind::Name)
    return decl->constraint->name.str();
  if (decl && decl->constraint && decl->constraint->kind == TypeRef::Kind::Union &&
      llvm::all_of(decl->constraint->args, [](const TypeRef &t) { return t.kind == TypeRef::Kind::Name; })) {
    std::string text;
    for (const TypeRef &t : decl->constraint->args)
      text += (text.empty() ? "" : " | ") + t.name.str();
    return text;
  }
  if (const Type *c = param->constraint())
    return c->name().str();
  std::string text;
  for (const Type *t : param->alternatives())
    text += (text.empty() ? "" : " | ") + t->name().str();
  return text;
}

// The parameters of the function type `fn` from position `from` on, as an
// argument list (see Type::argumentList).
const Type *paramsAsList(const Type *fn, size_t from) {
  llvm::ArrayRef<const Type *> params = fn->params();
  size_t fixed = params.size() - (fn->hasRest() ? 1 : 0);
  std::vector<const Type *> elements;
  for (size_t i = from; i < fixed; ++i)
    elements.push_back(params[i]);
  size_t min = fn->minArgs() > from ? fn->minArgs() - from : 0;
  return Type::argumentList(elements, std::min(min, elements.size()), fn->hasRest() ? params.back() : nullptr);
}

// The elements `[begin, end)` of an argument list, as one; the last part
// keeps the rest of the list, and the elements it may leave out.
const Type *listSlice(const Type *list, size_t begin, size_t end, bool last) {
  std::vector<const Type *> elements;
  for (size_t i = begin; i < end && i < list->fields().size(); ++i)
    elements.push_back(list->fields()[i].type);
  size_t min = elements.size();
  if (last)
    min = list->tupleMin() > begin ? std::min(list->tupleMin() - begin, elements.size()) : 0;
  return Type::argumentList(elements, min, last ? list->tupleRest() : nullptr);
}

// What the arguments of a call say about the type parameters of the callee:
// the types each one would be, unified later. Those of literals are weak:
// a literal takes the type its parameter gets, so `{ kind: "dog" }` next to
// a Pet makes T a Pet, and 2.5 next to an int makes it a double.
struct Inference {
  using Origins = llvm::DenseMap<const Type *, std::pair<TypeAliasDecl *, std::vector<const Type *>>>;
  // `origins` are the generic aliases that made object types, and their
  // type arguments: an interface of a generic one, `Repo<T>`, matches the
  // same one with other arguments.
  // `elementOf` gives the values iterating a type gives, for `xs:
  // Iterable<T>`.
  Inference(llvm::ArrayRef<const Type *> params, const Origins &origins,
            std::function<const Type *(const Type *)> elementOf)
      : params(params), candidates(params.size()), weakCandidates(params.size()),
        undefinedSeen(params.size(), false), origins(origins), elementOf(std::move(elementOf)) {}

  // Matches `pattern`, a type of the callee that mentions its type
  // parameters, against `actual`.
  void from(const Type *pattern, const Type *actual) {
    if (actual->isError() || actual->isVoid())
      return;
    if (auto at = llvm::find(params, pattern); at != params.end()) {
      size_t k = size_t(at - params.begin());
      if (actual->isUndefined()) {
        undefinedSeen[k] = true; // `T | undefined`, if anything else gives T
        return;
      }
      // `xs: Iterable<double>` given a literal `[1, 2]`: an array of the
      // values it wants, as a literal takes the type of its parameter.
      const Type *element = pattern->iterated();
      const Type *candidate = actual;
      if (element && weak && actual->isArray() && !mentionsParam(element))
        candidate = Type::array(element);
      (weak ? weakCandidates : candidates)[k].push_back(candidate);
      // `xs: Iterable<T>`: T is what iterating the argument gives.
      if (element && elementOf)
        if (const Type *given = elementOf(candidate))
          from(element, given);
      return;
    }
    if (!mentionsAnyOf(pattern, params))
      return;
    switch (pattern->kind()) {
    case Type::Kind::Optional:
      if (!actual->isUndefined())
        from(pattern->inner(), actual->nonOptional());
      return;
    case Type::Kind::Array:
      if (actual->isArray())
        from(pattern->inner(), actual->inner());
      return;
    case Type::Kind::Result:
      // `ok(1)` is a `Result<int, never>`: E is never there, as in
      // TypeScript, unless another argument gives it.
      if (actual->isResult()) {
        from(pattern->okType(), actual->okType());
        from(pattern->errType(), actual->errType());
      }
      return;
    case Type::Kind::Function: {
      if (!actual->isFunction())
        return;
      // `(...args: A) => R`: the parameters of the function from there on
      // are the list, with those it may leave out and its rest parameter.
      llvm::ArrayRef<const Type *> ps = pattern->params();
      bool list = pattern->hasRest() && (ps.back()->isListParam() || spreadsLists(ps.back()));
      size_t fixed = list ? ps.size() - 1 : ps.size();
      for (size_t i = 0; i < fixed && i < actual->params().size(); ++i)
        from(ps[i], actual->params()[i]);
      if (list && actual->params().size() - (actual->hasRest() ? 1 : 0) >= fixed)
        from(ps.back(), paramsAsList(actual, fixed));
      // A method that returns nothing gives `R = void`.
      if (auto at = llvm::find(params, pattern->ret()); at != params.end() && actual->ret()->isVoid())
        candidates[size_t(at - params.begin())].push_back(actual->ret());
      else if (!pattern->ret()->isVoid())
        from(pattern->ret(), actual->ret());
      return;
    }
    case Type::Kind::Iterator:
      if (!actual->isIterator())
        return;
      from(pattern->yielded(), actual->yielded());
      from(pattern->returned(), actual->returned());
      from(pattern->sent(), actual->sent());
      return;
    case Type::Kind::Dict:
      // An object literal goes where a Record is expected: its values say
      // the type of the Record's.
      if (pattern->isRecord() && actual->isObject() && !actual->isClass() && !actual->isTuple()) {
        for (const Field &f : actual->fields())
          from(pattern->valueType(), f.type);
        return;
      }
      if (!actual->isDict() || actual->dictKind() != pattern->dictKind())
        return;
      from(pattern->keyType(), actual->keyType());
      from(pattern->valueType(), actual->valueType());
      return;
    case Type::Kind::Promise:
      if (actual->isPromise())
        from(pattern->promised(), actual->promised());
      return;
    case Type::Kind::ClassRef:
      // `Constructor<T>` against a class, `typeof User`, or a Constructor:
      // T is what its objects are.
      if (pattern->isConstructor() && actual->isClassRef())
        from(pattern->constructed(),
             actual->isConstructor() ? actual->constructed() : actual->classRef()->type);
      return;
    case Type::Kind::Object: {
      // An async iterator, as an iterator (`AsyncIterable<T>`); an object
      // with `[Symbol.asyncIterator]()`, as what that gives.
      if (pattern->isAsyncIterator()) {
        if (!actual->isAsyncIterator() && asyncOf)
          if (const Type *given = asyncOf(actual))
            actual = given;
        if (actual->isAsyncIterator()) {
          from(pattern->yielded(), actual->yielded());
          from(pattern->returned(), actual->returned());
          from(pattern->sent(), actual->sent());
        }
        return;
      }
      // Recursive types meet themselves again.
      if (!actual->isObject() || !visited.insert({pattern, actual}).second)
        return;
      // `[X, ...A]` against an argument list: the parts of the list.
      if (spreadsLists(pattern) && actual->isTuple()) {
        matchList(pattern, actual, nullptr);
        return;
      }
      // `Box<T>` against `Box<int>`, or a class that extends one: the type
      // arguments of the instance.
      if (const ClassDecl *cls = pattern->classDecl(); cls && !pattern->typeArgs().empty()) {
        const ClassDecl *generic = cls->generic ? cls->generic : cls;
        if (const ClassDecl *found = genericInstanceOf(actual, generic))
          for (size_t k = 0; k < pattern->typeArgs().size() && k < found->typeArgs.size(); ++k)
            from(pattern->typeArgs()[k], found->typeArgs[k]);
        return;
      }
      // `Repo<T>` against a type that is `Repo<User>`, or is declared to be
      // (a class that implements it).
      if (auto origin = origins.find(pattern); origin != origins.end()) {
        std::vector<const Type *> queue{actual};
        for (size_t i = 0; i < queue.size(); ++i) {
          const Type *t = queue[i];
          auto other = origins.find(t);
          if (other != origins.end() && other->second.first == origin->second.first) {
            const std::vector<const Type *> &want = origin->second.second, &have = other->second.second;
            for (size_t k = 0; k < want.size() && k < have.size(); ++k)
              from(want[k], have[k]);
            return;
          }
          queue.insert(queue.end(), t->bases().begin(), t->bases().end());
        }
      }
      for (const Field &f : pattern->fields())
        if (const Field *a = actual->field(f.name))
          from(f.type, a->type);
      return;
    }
    case Type::Kind::Union: {
      // `T | string` against `int | string`: T is what the other members do
      // not hold.
      std::vector<const Type *> open, fixed;
      for (const Type *m : pattern->members())
        (mentionsAnyOf(m, params) ? open : fixed).push_back(m);
      if (open.size() != 1)
        return;
      std::vector<const Type *> rest;
      const Type *value = actual->nonOptional();
      for (const Type *m :
           value->isUnion() ? value->members() : llvm::ArrayRef<const Type *>(value))
        if (!llvm::is_contained(fixed, m))
          rest.push_back(m);
      if (!rest.empty())
        from(open.front(), Type::unionOf(rest));
      return;
    }
    default:
      return;
    }
  }

  // `[...A, X]`, `[...A, ...B]` against the argument list `actual`: each
  // part takes the elements its place in the list says. With two lists of
  // types, one must be known (`bound`, those found so far): what the other
  // arguments give is used first; until then the match waits (see retry).
  void matchList(const Type *pattern, const Type *actual, const std::vector<const Type *> *bound) {
    llvm::ArrayRef<Field> parts = pattern->fields();
    size_t count = actual->fields().size();
    // How many elements each part takes: one for a type, the length of a
    // known list, the rest for the one unknown list.
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
      if (value && value->isTuple() && !value->tupleRest() && value->tupleMin() == value->fields().size()) {
        lengths[i] = int(value->fields().size());
        known += value->fields().size();
      } else if (unknown >= 0) {
        if (!bound)
          deferred.push_back({pattern, actual});
        return;
      } else {
        unknown = int(i);
      }
    }
    // A rest part of the list goes only to the last part.
    if (known > count || (actual->tupleRest() && unknown != int(parts.size()) - 1))
      return;
    if (unknown >= 0)
      lengths[size_t(unknown)] = int(count - known);
    else if (known != count)
      return;
    size_t at = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
      size_t length = size_t(lengths[i]);
      if (parts[i].type->isListParam())
        from(parts[i].type, listSlice(actual, at, at + length, i + 1 == parts.size()));
      else if (at < count)
        from(parts[i].type, actual->fields()[at].type);
      at += length;
    }
  }

  // The matches of lists that waited for other arguments.
  void retry(const std::vector<const Type *> &bound) {
    std::vector<std::pair<const Type *, const Type *>> waiting = std::move(deferred);
    deferred.clear();
    for (auto [pattern, actual] : waiting)
      matchList(pattern, actual, &bound);
  }

  llvm::ArrayRef<const Type *> params;
  std::vector<std::vector<const Type *>> candidates, weakCandidates;
  std::vector<bool> undefinedSeen;
  std::set<std::pair<const Type *, const Type *>> visited;
  bool weak = false;
  const Origins &origins;
  std::function<const Type *(const Type *)> elementOf;
  // What `[Symbol.asyncIterator]()` of an object of a type gives, if any.
  std::function<const Type *(const Type *)> asyncOf;
  std::vector<std::pair<const Type *, const Type *>> deferred;
};

// An argument whose type depends on the type expected for it: an arrow whose
// parameters take their types from there, a generic function or a built-in
// one passed by name.
bool isContextSensitive(const Expr *arg) {
  if (auto *fe = dyn_cast<FunctionExpr>(arg))
    return llvm::any_of(fe->fn->params, [](const Param &p) { return !p.type; });
  // `[x => x + 1]`, `{ f: x => x }`: so are literals holding one.
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg))
    return llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return isContextSensitive(e.expr); });
  if (auto *lit = dyn_cast<ObjectLitExpr>(arg))
    return llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return isContextSensitive(p.value); });
  if (auto *name = dyn_cast<NameExpr>(arg))
    return name->typeArgs.empty() &&
           llvm::is_contained(
               llvm::ArrayRef<llvm::StringRef>{"String", "Number", "parseInt", "parseFloat",
                                               "isNaN", "isFinite", "Boolean", "encodeURIComponent",
                                               "encodeURI", "decodeURIComponent", "decodeURI"},
               name->name);
  if (auto *member = dyn_cast<MemberExpr>(arg))
    if (auto *object = dyn_cast<NameExpr>(member->object))
      return object->name == "Math" || object->name == "Number" || object->name == "String" ||
             object->name == "console";
  return false;
}

} // namespace

bool typeMentions(const Type *type, llvm::ArrayRef<const Type *> params) {
  return mentionsAnyOf(type, params);
}

//===----------------------------------------------------------------------===//
// Type parameters
//===----------------------------------------------------------------------===//

std::vector<const Type *> Sema::declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                                  std::vector<const Type *> *defaults) {
  std::vector<Type *> created;
  for (size_t i = 0; i < decls.size(); ++i) {
    for (size_t j = 0; j < i; ++j)
      if (decls[j].name == decls[i].name)
        diags_.error(decls[i].loc, "duplicate type parameter '" + decls[i].name + "'");
    if (Type::fromName(decls[i].name))
      diags_.error(decls[i].loc, "'" + decls[i].name + "' is a built-in type and cannot name a type "
                                 "parameter");
    created.push_back(Type::param(decls[i].name));
  }
  std::vector<const Type *> params(created.begin(), created.end());
  bindTypeParams(decls, params);

  bool afterDefault = false;
  for (size_t i = 0; i < decls.size(); ++i) {
    const TypeParamDecl &decl = decls[i];
    if (decl.constraint) {
      const TypeRef &ref = *decl.constraint;
      const Type *constraint = nullptr;
      std::vector<const Type *> alternatives;
      bool ok = true;
      if (ref.kind == TypeRef::Kind::Array && ref.args[0].kind == TypeRef::Kind::Name &&
          ref.args[0].name == "unknown") {
        // `A extends unknown[]`: the types of the parameters of some
        // function, a list of them (section 9); `unknown[]` means that only
        // here.
        created[i]->setList();
      } else if (ref.kind == TypeRef::Kind::Name && ref.name == "unknown") {
        // Any type at all, which is what no constraint says.
      } else if (ref.kind == TypeRef::Kind::Union) {
        // `T extends string | int`: an instance for each.
        for (const TypeRef &member : ref.args) {
          const Type *type = resolveType(member, /*allowVoid=*/false);
          ok &= !type->isError();
          if (!llvm::is_contained(alternatives, type))
            alternatives.push_back(type);
        }
        // `T extends int | undefined` is one type.
        auto isUndefined = [](const Type *t) { return t->isUndefined(); };
        if (ok && alternatives.size() == 2 && llvm::any_of(alternatives, isUndefined))
          constraint = Type::optional(alternatives[alternatives[0]->isUndefined() ? 1 : 0]);
      } else {
        constraint = resolveType(ref, /*allowVoid=*/false);
        ok &= !constraint->isError();
      }
      llvm::ArrayRef<const Type *> all =
          constraint ? llvm::ArrayRef<const Type *>(constraint) : llvm::ArrayRef<const Type *>(alternatives);
      for (const Type *type : all) {
        if (!ok)
          break;
        if (type->isUndefined()) {
          diags_.error(ref.loc, "the constraint of '" + decl.name + "' cannot be 'undefined' alone");
          ok = false;
        }
      }
      if (ok)
        created[i]->setConstraint(constraint, constraint ? std::vector<const Type *>() : alternatives);
    }
    const Type *fallback = nullptr;
    if (decl.defaultType) {
      fallback = resolveType(*decl.defaultType, /*allowVoid=*/false);
      if (!fallback->isError() && !satisfies(fallback, params[i])) {
        diags_.error(decl.defaultType->loc, "the default '" + fallback->name() + "' of '" + decl.name +
                                                "' does not satisfy its constraint '" +
                                                constraintSpelling(params[i], &decl) + "'");
        fallback = Type::error();
      }
      afterDefault = true;
    } else if (afterDefault) {
      diags_.error(decl.loc, "type parameter '" + decl.name +
                                 "' needs a default: it follows one that has one");
    }
    if (defaults)
      defaults->push_back(fallback);
  }
  return params;
}

void Sema::bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls, llvm::ArrayRef<const Type *> types) {
  for (size_t i = 0; i < decls.size() && i < types.size(); ++i) {
    auto *alias = ctx_.make<TypeAliasDecl>(decls[i].loc, decls[i].name, TypeRef());
    alias->resolved = types[i];
    typeScopes_.back()[decls[i].name] = alias;
  }
}

const Type *Sema::substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                             llvm::ArrayRef<const Type *> args) {
  if (!mentionsAnyOf(type, params))
    return type;
  auto sub = [&](const Type *t) { return substitute(t, params, args); };
  switch (type->kind()) {
  case Type::Kind::Param: {
    // `T[K]` and the other computations, once their operands are known.
    if (type->typeOp() != TypeOp::None) {
      std::vector<const Type *> operands;
      for (const Type *t : type->operands())
        operands.push_back(t ? sub(t) : nullptr);
      return evalTypeOp(type, operands);
    }
    const Type *arg = args[size_t(llvm::find(params, type) - params.begin())];
    return arg ? arg : type; // not known yet
  }
  case Type::Kind::Optional:
    return Type::optional(sub(type->inner()));
  case Type::Kind::Array:
    return Type::array(sub(type->inner()));
  case Type::Kind::Result:
    return Type::result(sub(type->okType()), sub(type->errType()));
  case Type::Kind::Function: {
    std::vector<const Type *> ps;
    for (const Type *p : type->params())
      ps.push_back(sub(p));
    return Type::function(ps, type->minArgs(), type->hasRest(), sub(type->ret()), type->hasThis());
  }
  case Type::Kind::Iterator:
    return Type::iterator(sub(type->yielded()), sub(type->returned()), sub(type->sent()));
  case Type::Kind::Dict:
    return Type::dict(type->dictKind(), sub(type->keyType()), sub(type->valueType()));
  case Type::Kind::Union: {
    std::vector<const Type *> members;
    for (const Type *m : type->members())
      members.push_back(sub(m));
    return Type::unionOf(members);
  }
  case Type::Kind::Promise:
    return Type::promise(sub(type->promised()));
  case Type::Kind::ClassRef: // `Constructor<T>`
    return type->isConstructor() ? Type::constructorOf(sub(type->constructed())) : type;
  case Type::Kind::Object: {
    if (type->isAsyncIterator())
      return Type::asyncIterator(sub(type->yielded()), sub(type->returned()), sub(type->sent()));
    // `MethodDescriptor<A, R>` keeps its name.
    if (auto origin = descriptorOrigins_.find(type); origin != descriptorOrigins_.end()) {
      auto [list, ret] = origin->second;
      return methodDescriptorType(sub(list), sub(ret));
    }
    // A class made from a generic one is the instance for the other
    // arguments.
    if (const ClassDecl *cls = type->classDecl(); cls && !type->typeArgs().empty()) {
      std::vector<const Type *> newArgs;
      for (const Type *a : type->typeArgs())
        newArgs.push_back(sub(a));
      // Where the instance is first needed in the program, if anywhere, is
      // where its errors are reported (see instantiateClass).
      auto *generic = const_cast<ClassDecl *>(cls->generic ? cls->generic : cls);
      ClassDecl *inst = instantiateClass(generic, std::move(newArgs), instanceLoc_);
      return inst ? inst->type : Type::error();
    }
    // An instance of a generic alias is that alias with other arguments; a
    // recursive one cannot be rebuilt field by field.
    if (auto origin = aliasOrigins_.find(type); origin != aliasOrigins_.end()) {
      // Copied: substituting may add entries.
      auto [alias, oldArgs] = origin->second;
      std::vector<const Type *> newArgs;
      for (const Type *a : oldArgs)
        newArgs.push_back(sub(a));
      return instantiateAlias(alias, std::move(newArgs), alias->loc());
    }
    // `[int, T]` stays a tuple; `[int, ...A]` takes the types A stands for,
    // and an argument list keeps the elements that may be left out.
    if (type->isTuple()) {
      std::vector<const Type *> elements;
      std::vector<bool> spread;
      for (const Field &f : type->fields()) {
        elements.push_back(sub(f.type));
        spread.push_back(f.type->isListParam());
      }
      if (llvm::is_contained(spread, true)) {
        std::string why;
        const Type *list = spreadTuple(elements, spread, why);
        return list ? list : Type::error();
      }
      if (type->tupleMin() < elements.size() || type->tupleRest())
        return Type::argumentList(elements, type->tupleMin(), type->tupleRest() ? sub(type->tupleRest()) : nullptr);
      return Type::tuple(elements, !type->fields().empty() && type->fields().front().readonly);
    }
    if (!substituting_.insert(type).second)
      return type;
    std::vector<Field> fields;
    for (const Field &f : type->fields())
      fields.push_back({f.name, sub(f.type), f.readonly});
    substituting_.erase(type);
    return Type::object(std::move(fields));
  }
  default:
    return type;
  }
}

const Type *Sema::representation(const Type *type) {
  switch (type->kind()) {
  // An iterator and a dictionary are objects of the runtime, released
  // through their own descriptors as objects are.
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return Type::anyObject();
  case Type::Kind::Function:
    return Type::function({}, 0, false, Type::voidTy());
  case Type::Kind::Array:
    return Type::array(representation(type->inner()));
  case Type::Kind::Optional:
    return Type::optional(representation(type->inner()));
  case Type::Kind::Result: // its halves lie as such values do
    return Type::result(representation(type->okType()), representation(type->errType()));
  case Type::Kind::Union: {
    // The objects of a union are one reference; the tags of the other
    // members stay.
    std::vector<const Type *> members;
    for (const Type *m : type->members())
      members.push_back(representation(m));
    return Type::unionOf(members);
  }
  default:
    return type;
  }
}

bool Sema::satisfies(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params,
                     llvm::ArrayRef<const Type *> args) {
  if (arg->isError())
    return true;
  // `K extends keyof T`: with the type T stands for.
  const Type *own = param->constraint();
  if (own && !params.empty() && mentionsAnyOf(own, params))
    own = substitute(own, params, args);
  if (own && own->isError())
    return true;
  // A type parameter whose constraint is as strict.
  if (arg->isParam() && arg->constraint() && own && isImplicitlyConvertible(arg->constraint(), own))
    return true;
  auto fits = [&](const Type *constraint) {
    return isImplicitlyConvertible(arg, constraint) ||
           (constraint->isObject() && hasMembersOf(arg, constraint));
  };
  if (own)
    return fits(own);
  llvm::ArrayRef<const Type *> alternatives = param->alternatives();
  return alternatives.empty() || llvm::any_of(alternatives, fits);
}

bool Sema::hasMembersOf(const Type *arg, const Type *constraint) {
  if (arg->isParam() && arg->constraint())
    return hasMembersOf(arg->constraint(), constraint);
  if (!arg->isObject() || arg->isTuple() != constraint->isTuple() || constraint->isClass() ||
      arg->isAnyObject())
    return false;
  if (ClassDecl *cls = classOf(arg))
    completeClass(cls);
  const ClassDecl *cls = arg->classDecl();
  for (const Field &f : constraint->fields()) {
    if (const Field *have = arg->field(f.name)) {
      // A read-only field of the constraint may be one that code changes.
      if (have->type != f.type || (have->readonly && !f.readonly))
        return false;
      continue;
    }
    // A method of a class, for a field of the constraint that holds a
    // function and cannot be replaced, `greet(): string`.
    bool method = false;
    if (cls && f.readonly && f.type->isFunction())
      for (FunctionDecl *fn : methodsNamed(cls, f.name, /*isStatic=*/false))
        method |= fn->sym && fn->typeParams.empty() && functionType(fn->sym) == f.type;
    if (!method && !f.type->isOptional())
      return false;
  }
  for (const Overload &overload : constraint->overloads()) {
    if (!cls)
      return false;
    std::vector<FunctionDecl *> methods = methodsNamed(cls, overload.name, /*isStatic=*/false);
    for (const Type *signature : overload.signatures)
      if (llvm::none_of(methods, [&](FunctionDecl *fn) { return fn->sym && functionType(fn->sym) == signature; }))
        return false;
  }
  return true;
}

bool Sema::exactTypeParam(FuncSymbol *generic, size_t k) {
  auto found = exactParams_.find(generic);
  if (found == exactParams_.end()) {
    llvm::ArrayRef<const Type *> params = generic->typeParams;
    std::vector<bool> exact(params.size(), false);
    for (size_t i = 0; i < params.size(); ++i) {
      // The body may read the fields of an object.
      auto objectLike = [](const Type *t) { return t && t->nonOptional()->isObject(); };
      exact[i] = objectLike(params[i]->constraint()) || llvm::any_of(params[i]->alternatives(), objectLike);
      // And iterates the type its own way: `xs: Iterable<int>`, and what it
      // gives, `xs: Iterable<T>`; a list of the types of parameters is the
      // parameters of the copy.
      exact[i] = exact[i] || params[i]->iterated() || params[i]->isListParam();
      for (const Type *p : params)
        exact[i] = exact[i] || (p->iterated() && mentionsAnyOf(p->iterated(), params[i]));
      // The keys of T: `K extends keyof T`, and `T[K]`, read the fields of
      // T by name.
      for (const Type *p : params)
        exact[i] = exact[i] || (p != params[i] && p->constraint() && mentionsAnyOf(p->constraint(), params[i]));
      // An instance of a generic class is a class of its own, and an
      // interface made of a generic one an interface of its own: a copy that
      // makes or takes `Box<T>` must make and take that of its own type.
      llvm::SmallPtrSet<const Type *, 4> seen;
      for (const Type *p : generic->params)
        exact[i] = exact[i] || (p && mentionsInObject(p, params[i], seen));
      if (generic->ret)
        exact[i] = exact[i] || mentionsInObject(generic->ret, params[i], seen);
    }
    found = exactParams_.try_emplace(generic, std::move(exact)).first;
  }
  return k < found->second.size() && found->second[k];
}

const Type *Sema::apparentType(const Type *type) {
  if (type->isParam() && type->constraint() && !type->isListParam())
    return type->constraint();
  // A literal type has the operations of its base: `dir.length`, `bit + 1`.
  if (type->isLiteral())
    return type->literalBase();
  if (type->isUnion() && llvm::all_of(type->members(), [&](const Type *m) {
        return m->isLiteral() && m->literalBase() == type->members().front()->literalBase();
      }))
    return type->members().front()->literalBase();
  return type;
}

Expr *Sema::apparent(Expr *expr) {
  const Type *type = apparentType(expr->type);
  return type == expr->type ? expr : convert(expr, type);
}

FuncSymbol *Sema::genericNamed(const NameExpr *name) const {
  ScopeEntry entry = lookup(name->name);
  return entry.func && !entry.var && entry.func->isGeneric() ? entry.func : nullptr;
}

const Type *Sema::numberParam(const Type *param) const {
  const Type *&number = numberParams_[param];
  if (!number) {
    // The numbers it may be: double without a constraint, else the widest
    // of its numeric alternatives.
    const Type *constraint = nullptr;
    for (const Type *t : param->alternatives())
      if (t->isNumeric())
        constraint = constraint ? commonNumericType(constraint, t) : t;
    Type *type = Type::param(param->name());
    type->setConstraint(constraint ? constraint : Type::doubleTy(), {});
    number = type;
  }
  return number;
}

const Type *Sema::narrowedType(const VarSymbol *var) const {
  for (const auto &[narrowed, type] : flow_.narrowed)
    if (narrowed == var)
      return type;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// What instances convert as the generic code does
//===----------------------------------------------------------------------===//

void Sema::noteForced(const Expr *expr, const Type *to) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  forced_[{expr->loc().getPointer(), unsigned(expr->kind())}] = to;
}

Expr *Sema::forceConversion(Expr *expr, const Type *to) {
  const Type *type = expr->type;
  if (!type || type->isError() || type == to)
    return expr;
  if (to->isString())
    return toStringOperand(expr); // reports an object or a function
  if (isImplicitlyConvertible(type, to))
    return convert(expr, to);
  return expr;
}

std::optional<bool> Sema::constantCondition(const Expr *cond) const {
  while (auto *conv = dyn_cast<CastExpr>(cond)) {
    if (!conv->isImplicit() || !conv->type->isBool() || !conv->operand->type->isBool())
      break;
    cond = conv->operand;
  }
  if (auto *lit = dyn_cast<BoolLitExpr>(cond))
    return lit->value;
  if (auto *unary = dyn_cast<UnaryExpr>(cond); unary && unary->op == UnaryOp::Not) {
    if (std::optional<bool> value = constantCondition(unary->operand))
      return !*value;
    return std::nullopt;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin)
    return std::nullopt;
  if (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr) {
    bool isAnd = bin->op == BinaryOp::LogAnd;
    std::optional<bool> l = constantCondition(bin->lhs);
    if (l && *l != isAnd)
      return l; // `false && x`, `true || x`
    if (l)
      return constantCondition(bin->rhs);
    return std::nullopt;
  }
  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne)
    return std::nullopt;
  // `typeof x == "string"`, with x a variable: its type tells, unless it may
  // be undefined.
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text)
      continue;
    const Expr *operand = typeOf->operand;
    while (isa<CastExpr>(operand) && cast<CastExpr>(operand)->isImplicit())
      operand = cast<CastExpr>(operand)->operand;
    if (!isa<NameExpr>(operand))
      return std::nullopt;
    const Type *type = typeOf->operand->type;
    llvm::StringRef name = typeOfName(type->nonOptional());
    if (name.empty()) // a union of several kinds: known when it runs
      return std::nullopt;
    std::u16string wanted(name.begin(), name.end());
    bool equal;
    if (type->isOptional()) {
      if (text->value == u"undefined" || text->value == wanted)
        return std::nullopt;
      equal = false;
    } else {
      equal = text->value == wanted;
    }
    return bin->op == BinaryOp::Eq ? equal : !equal;
  }
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Calls and values
//===----------------------------------------------------------------------===//

const Type *Sema::literalShape(const Expr *expr) {
  if (const Type *type = literalType(expr))
    return isa<TemplateExpr>(expr) ? nullptr : type;
  if (isa<UndefinedExpr>(expr))
    return Type::undefinedTy();
  if (auto *lit = dyn_cast<ArrayLitExpr>(expr)) {
    const Type *element = nullptr;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      const Type *type = e.spread || e.hole ? nullptr : literalShape(e.expr);
      element = type && element ? unify(element, type) : type;
      if (!element)
        return nullptr;
    }
    return element && !element->isUndefined() ? Type::array(element) : nullptr;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    std::vector<Field> fields;
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Type *type = literalShape(p.value);
      if (!type || type->isUndefined() ||
          llvm::any_of(fields, [&](const Field &f) { return f.name == p.name; }))
        return nullptr;
      fields.push_back({p.name.str(), type, false});
    }
    return Type::object(std::move(fields));
  }
  return nullptr;
}

bool Sema::finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound, llvm::SMLoc loc, bool call,
                          bool quiet) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  bool ok = true;
  for (size_t k = 0; k < params.size(); ++k) {
    // A default may use the type parameters before it, once they are known.
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k]) {
      const Type *fallback = substitute(generic->typeDefaults[k], params, bound);
      bool open = false;
      for (size_t j = 0; j < params.size(); ++j)
        open |= !bound[j] && mentionsAnyOf(fallback, params[j]);
      if (!open)
        bound[k] = fallback;
    }
    if (bound[k] && bound[k]->isError()) {
      ok = false;
      continue;
    }
    if (!bound[k] && quiet) { // an argument has an error already
      bound[k] = Type::error();
      ok = false;
      continue;
    }
    if (!bound[k]) {
      // tsc would make it `unknown`, which tz does not have.
      // A list of the types of parameters is a tuple of them.
      std::string ints;
      for (size_t i = 0; i < params.size(); ++i)
        ints += (i == 0 ? "" : ", ") + std::string(params[i]->isListParam() ? "[int]" : "int");
      diags_.error(loc, "cannot infer type parameter '" + params[k]->name() + "' of '" + generic->name +
                            "': nothing " + (call ? "in the call" : "here") +
                            " says what it is; give it, e.g. '" + generic->name + "<" + ints + ">" +
                            (call ? "(...)" : "") + "'");
      bound[k] = Type::error();
      ok = false;
      continue;
    }
    // `xs: Iterable<int>`: something iteration gives ints of.
    if (const Type *element = params[k]->iterated()) {
      const Type *wanted = substitute(element, params, bound);
      const Type *given = elementTypeOf(bound[k]);
      if (given == wanted || wanted->isError() || bound[k]->isError())
        continue;
      diags_.error(loc, given ? "'" + bound[k]->name() + "' gives values of type '" + given->name() +
                                    "', and the parameter 'Iterable<" + wanted->name() + ">' of '" +
                                    generic->name + "' takes values of type '" + wanted->name() + "'"
                              : "'" + bound[k]->name() + "' cannot be iterated, and the parameter "
                                "'Iterable<" + wanted->name() + ">' of '" + generic->name +
                                "' takes something that can: an array, a string, a Map, a Set, an "
                                "iterator, or an object with '[Symbol.iterator]()'");
      bound[k] = Type::error();
      ok = false;
      continue;
    }
    if (!satisfies(bound[k], params[k], params, bound)) {
      llvm::ArrayRef<TypeParamDecl> decls = generic->decl->typeParams;
      diags_.error(loc, "type '" + bound[k]->name() + "' does not satisfy the constraint '" +
                            constraintSpelling(params[k], k < decls.size() ? &decls[k] : nullptr) + "' of '" +
                            params[k]->name() + "' in '" + generic->name + "'");
      bound[k] = Type::error();
      ok = false;
    }
  }
  return ok;
}

Expr *Sema::checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                             llvm::ArrayRef<TypeRef> typeArgs) {
  std::vector<const Type *> bound;
  if (!bindGenericCall(call, generic, loc, expected, typeArgs, bound))
    return call;
  call->func = generic;
  // In generic code a call with type parameters is checked only; so is one
  // whose callee's return type is still being found, and a method of a
  // generic class itself, or of an instance that generic code uses (not a
  // static one: the generic class has those).
  const ClassDecl *cls = generic->decl ? generic->decl->cls : nullptr;
  bool concrete = llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); }) &&
                  (!cls || generic->decl->isStatic || cls->isEmitted());
  bool copiesReturn = isTemplate(generic) && !generic->ret;
  if (concrete && !trial_ && !generic->inferring && (generic->ret || copiesReturn))
    if (FuncSymbol *instance = instantiate(generic, bound, loc, exactClassArgs(call, generic))) {
      call->func = instance;
      // A copy of a template has its own return type: the class it makes.
      if (copiesReturn)
        call->type = instance->ret ? instance->ret : Type::error();
    }
  return call;
}

bool Sema::bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc, const Type *expected,
                           llvm::ArrayRef<TypeRef> typeArgs, std::vector<const Type *> &bound) {
  call->type = Type::error();
  unsigned errorsBefore = diags_.errorsSeen();
  resolveParams(generic);
  const Type *ret = returnType(generic);
  if (returnsOwnClass(generic, ret))
    ret = nullptr;
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  for (size_t i = 0; i < call->args.size() && i < (generic->hasRest ? fixed : call->args.size()); ++i)
    if (call->spreads(i) && rejectSpread(call))
      return false;
  if (!checkArity(call, loc, static_cast<unsigned>(generic->minArgs),
                  generic->hasRest ? ~0u : static_cast<unsigned>(generic->params.size())))
    return false;
  // The parameter an argument goes to: the elements of a rest parameter, or
  // the whole array for one spread there.
  auto paramOf = [&](size_t i) -> const Type * {
    if (!generic->hasRest || i < fixed)
      return generic->params[i];
    const Type *rest = generic->params.back();
    return call->spreads(i) || !rest->isArray() ? rest : rest->inner();
  };
  // `...head: A`: the arguments from there on make the list A stands for.
  const Type *listRest = generic->hasRest && (generic->params.back()->isListParam() ||
                                              spreadsLists(generic->params.back()))
                             ? generic->params.back()
                             : nullptr;

  bound.assign(typeParams.size(), nullptr);
  std::vector<bool> checked(call->args.size(), false);
  std::vector<size_t> literals; // checked once their types are known
  bool ok = true;
  if (!typeArgs.empty()) {
    if (typeArgs.size() > typeParams.size()) {
      diags_.error(loc, "'" + generic->name + "' takes " + std::to_string(typeParams.size()) +
                            " type argument" + (typeParams.size() == 1 ? "" : "s") + ", but got " +
                            std::to_string(typeArgs.size()));
      ok = false;
    }
    for (size_t k = 0; k < typeArgs.size() && k < typeParams.size(); ++k)
      bound[k] = resolveType(typeArgs[k], /*allowVoid=*/false);
  } else {
    Inference inference(typeParams, aliasOrigins_, [this](const Type *t) { return elementTypeOf(t); });
    inference.asyncOf = [this](const Type *t) { return asyncIterableType(t); };
    auto settle = [&](size_t k) {
      if (bound[k])
        return;
      const Type *type = nullptr;
      bool strong = !inference.candidates[k].empty();
      for (const std::vector<const Type *> *list : {&inference.candidates[k], &inference.weakCandidates[k]}) {
        for (const Type *c : *list) {
          const Type *next = type ? unify(type, c) : c;
          // A literal that does not fit is checked against the type the
          // other arguments give, and reported there.
          if (!next && strong && list == &inference.weakCandidates[k])
            continue;
          if (!next) {
            diags_.error(loc, "type parameter '" + typeParams[k]->name() + "' of '" + generic->name +
                                  "' gets '" + type->name() + "' from one argument and '" + c->name() +
                                  "' from another; give it, e.g. '" + generic->name + "<" + type->name() +
                                  ">(...)'");
            bound[k] = Type::error();
            ok = false;
            return;
          }
          type = next;
        }
      }
      if (type && inference.undefinedSeen[k])
        type = Type::optional(type);
      bound[k] = type;
    };

    // The arguments, except those whose types come from the type parameters:
    // literals take them once they are known (`pair(1, 2.5)` makes T a double
    // and 1 a double too), arrows after all the others.
    std::vector<size_t> sensitive;
    // `ok` and `err` passed by name (`mapAll(xs, ok)`) are functions only
    // where a function type is expected.
    auto makesResults = [&](const Expr *arg) {
      auto *name = dyn_cast<NameExpr>(arg);
      llvm::StringRef which = name ? neverthrowName(name->name) : llvm::StringRef();
      return which == "ok" || which == "err";
    };
    for (size_t i = 0; i < call->args.size(); ++i) {
      Expr *&arg = call->args[i];
      const Type *pattern = paramOf(i);
      if (listRest && i >= fixed) {
        arg = checkExpr(arg);
        checked[i] = true;
        continue;
      }
      if (!mentionsAnyOf(pattern, typeParams)) {
        arg = checkExpr(arg, pattern);
        checked[i] = true;
      } else if (auto *text = dyn_cast<StringLitExpr>(arg);
                 text && llvm::is_contained(typeParams, pattern) && wantsLiteral(pattern)) {
        // `get(user, "name")` with `K extends keyof T`: K = "name".
        const Type *literal = Type::stringLiteral(text->value);
        arg = checkExpr(arg, literal);
        checked[i] = true;
        inference.from(pattern, literal);
      } else if (isContextSensitive(arg) || makesResults(arg) ||
                 (isa<NameExpr>(arg) && genericNamed(cast<NameExpr>(arg)) &&
                  cast<NameExpr>(arg)->typeArgs.empty())) {
        sensitive.push_back(i);
      } else if (const Type *shape = literalShape(arg)) {
        literals.push_back(i);
        inference.weak = true;
        inference.from(pattern, shape);
        inference.weak = false;
      } else if (isa<ArrayLitExpr>(arg) || isa<ObjectLitExpr>(arg)) {
        // `{ title: t, tags: [] }`: checked with the type its parameter
        // gets from the others, or else for the types it gives.
        sensitive.push_back(i);
      } else {
        arg = checkExpr(arg);
        checked[i] = true;
        inference.from(pattern, arg->type);
      }
    }
    if (listRest) {
      std::vector<const Type *> parts;
      std::vector<bool> spread;
      for (size_t i = fixed; i < call->args.size(); ++i) {
        parts.push_back(call->args[i]->type);
        spread.push_back(call->spreads(i));
      }
      std::string why;
      if (const Type *list = spreadTuple(parts, spread, why))
        inference.from(listRest, list);
    }
    for (size_t k = 0; k < typeParams.size(); ++k)
      if (!inference.candidates[k].empty())
        settle(k);
    // `[...A, ...B]` of an argument once A is known from another one.
    if (!inference.deferred.empty()) {
      inference.retry(bound);
      for (size_t k = 0; k < typeParams.size(); ++k)
        if (!inference.candidates[k].empty())
          settle(k);
    }
    // `const names: string[] = emptyList()`; the type expected also comes
    // before literals: `const xs: double[] = listOf(1, 2)`.
    if (expected && llvm::is_contained(bound, nullptr) && ret && mentionsAnyOf(ret, typeParams)) {
      Inference fromExpected(typeParams, aliasOrigins_, nullptr);
      fromExpected.from(ret, expected);
      for (size_t k = 0; k < typeParams.size(); ++k) {
        if (bound[k] || fromExpected.candidates[k].empty())
          continue;
        const Type *type = nullptr;
        for (const Type *c : fromExpected.candidates[k])
          type = type ? unify(type, c) : c;
        bound[k] = type;
      }
    }
    for (size_t k = 0; k < typeParams.size(); ++k)
      settle(k);
    // Arrows: their parameters get the types known by now, and what they
    // return gives the rest (`mapAll(xs, x => x * 2)`).
    for (size_t i : sensitive) {
      Expr *&arg = call->args[i];
      const Type *pattern = paramOf(i);
      const Type *partial = substitute(pattern, typeParams, bound);
      const Type *fn = partial->nonOptional();
      if (fn->isFunction()) {
        Signature sig;
        for (const Type *p : fn->params())
          sig.params.push_back(mentionsAnyOf(p, typeParams) ? Type::error() : p);
        sig.minArgs = fn->minArgs();
        sig.ret = mentionsAnyOf(fn->ret(), typeParams) ? nullptr : fn->ret();
        sig.voidResult = fn->ret()->isVoid();
        auto *name = dyn_cast<NameExpr>(arg);
        auto *fe = dyn_cast<FunctionExpr>(arg);
        // The parameters it takes from here need the types known by now
        // (`run(x => ...)` with nothing else giving T); a generic function
        // passed by name reports what it misses itself.
        for (size_t p = 0; p < fn->params().size() && !(name && genericNamed(name)); ++p) {
          if (fe && p < fe->fn->params.size() && fe->fn->params[p].type)
            continue;
          for (size_t k = 0; k < typeParams.size(); ++k) {
            if (bound[k] || !mentionsAnyOf(fn->params()[p], typeParams[k]))
              continue;
            std::string what = fe && p < fe->fn->params.size()
                                   ? "parameter '" + fe->fn->params[p].name.str() + "' of this function"
                                   : "the parameters of this function";
            diags_.error(arg->loc(), "cannot infer type parameter '" + typeParams[k]->name() + "' of '" +
                                         generic->name + "' for " + what +
                                         ": nothing else in the call says what it is; give it, e.g. '" +
                                         generic->name + "<int>(...)'" +
                                         (fe ? ", or the type of the parameter" : ""));
            bound[k] = Type::error();
            ok = false;
          }
        }
        if (fe)
          arg = checkFunctionExpr(fe, &sig);
        else if (FuncSymbol *other = name ? genericNamed(name) : nullptr)
          arg = checkGenericValue(name, other, &sig);
        else if (FunctionExpr *wrapper = builtinFunctionValue(arg, sig, unsigned(sig.params.size()), {}))
          arg = checkFunctionExpr(wrapper, &sig);
        else
          arg = checkExpr(arg);
      } else {
        // An object literal with arrows, `new ReadableStream({ start: c =>
        // ... })`: what is still unknown takes its default, which the
        // arrows then see.
        bool defaulted = false;
        for (size_t k = 0; k < typeParams.size() && mentionsAnyOf(partial, typeParams); ++k)
          if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k] &&
              !mentionsAnyOf(generic->typeDefaults[k], typeParams)) {
            bound[k] = generic->typeDefaults[k];
            defaulted = true;
          }
        if (defaulted)
          partial = substitute(pattern, typeParams, bound);
        arg = checkExpr(arg, mentionsAnyOf(partial, typeParams) ? nullptr : partial);
      }
      checked[i] = true;
      Inference more(typeParams, aliasOrigins_, [this](const Type *t) { return elementTypeOf(t); });
      more.from(pattern, arg->type);
      for (size_t k = 0; k < typeParams.size(); ++k)
        if (!bound[k]) {
          inference.candidates[k] = more.candidates[k];
          inference.weakCandidates[k].clear();
          inference.undefinedSeen[k] = more.undefinedSeen[k];
          settle(k);
        }
    }
  }

  // An argument with an error leaves a type parameter unknown; that error
  // says enough, if there was one.
  bool argErrors = false;
  for (size_t i = 0; i < call->args.size(); ++i)
    argErrors |= checked[i] && call->args[i]->type->isError();
  argErrors &= diags_.errorsSeen() > errorsBefore;
  ok = finishTypeArgs(generic, bound, loc, /*call=*/true, /*quiet=*/argErrors) && ok;
  // `f(x: T)` calling `f([x])` from its own body: each instance needs a
  // bigger one.
  for (size_t k = 0; k < typeParams.size() && ok; ++k) {
    if (!grows(bound[k], typeParams, false))
      continue;
    diags_.error(loc, "'" + generic->name + "' calls itself with '" + bound[k]->name() + "' for '" +
                          typeParams[k]->name() + "', so each copy of it for some type needs another for a "
                          "bigger one, without end; a generic function can call itself only with its own "
                          "type parameters");
    ok = false;
  }
  if (!ok) {
    for (size_t i = 0; i < call->args.size(); ++i)
      if (!checked[i])
        call->args[i] = checkExpr(call->args[i]);
    return false;
  }

  // The instances of generic classes that the types of the call name are
  // first used here.
  llvm::SMLoc outerLoc = std::exchange(instanceLoc_, loc);
  std::vector<const Type *> params;
  for (const Type *p : generic->params)
    params.push_back(substitute(p, typeParams, bound));
  for (size_t i : literals) {
    call->args[i] = checkExpr(call->args[i], substitute(paramOf(i), typeParams, bound));
    checked[i] = true;
  }
  call->type = ret ? substitute(ret, typeParams, bound) : Type::error();
  instanceLoc_ = outerLoc;
  // `...head: A` with A = [int, int]: two parameters, as the copy has.
  size_t minArgs = generic->minArgs;
  bool hasRest = generic->hasRest;
  std::vector<const Type *> flat;
  if (flattenArguments(params, minArgs, hasRest, flat))
    params = std::move(flat);
  checkArguments(call, params, minArgs, hasRest, loc, typeArgs.empty() ? &checked : nullptr);
  return ret != nullptr;
}

bool Sema::inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args, std::vector<const Type *> &bound) {
  resolveParams(generic);
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  Inference inference(typeParams, aliasOrigins_, [this](const Type *t) { return elementTypeOf(t); });
  inference.asyncOf = [this](const Type *t) { return asyncIterableType(t); };
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  for (size_t i = 0; i < args.size(); ++i) {
    const Type *pattern = nullptr;
    if (i < fixed)
      pattern = generic->params[i];
    else if (generic->hasRest && generic->params.back()->isArray())
      pattern = generic->params.back()->inner();
    if (pattern && args[i]->type)
      inference.from(pattern, args[i]->type);
  }
  bound.assign(typeParams.size(), nullptr);
  for (size_t k = 0; k < typeParams.size(); ++k) {
    const Type *type = nullptr;
    for (const Type *c : inference.candidates[k]) {
      type = type ? unify(type, c) : c;
      if (!type)
        return false;
    }
    if (type && inference.undefinedSeen[k])
      type = Type::optional(type);
    bound[k] = type;
  }
  // The defaults, quietly: a variant that does not fit is left out.
  for (size_t k = 0; k < typeParams.size(); ++k) {
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k])
      bound[k] = substitute(generic->typeDefaults[k], typeParams, bound);
    if (!bound[k] || mentionsAnyOf(bound[k], typeParams) || !satisfies(bound[k], typeParams[k], typeParams, bound))
      return false;
  }
  return true;
}

Expr *Sema::checkGenericValue(NameExpr *name, FuncSymbol *generic, const Signature *expected) {
  name->type = Type::error();
  resolveParams(generic);
  const Type *ret = returnType(generic);
  if (returnsOwnClass(generic, ret))
    ret = nullptr;
  llvm::ArrayRef<const Type *> typeParams = generic->typeParams;
  std::vector<const Type *> bound(typeParams.size(), nullptr);
  if (!name->typeArgs.empty()) {
    if (name->typeArgs.size() > typeParams.size()) {
      diags_.error(name->loc(), "'" + generic->name + "' takes " + std::to_string(typeParams.size()) +
                                    " type argument" + (typeParams.size() == 1 ? "" : "s") + ", but got " +
                                    std::to_string(name->typeArgs.size()));
      return name;
    }
    for (size_t k = 0; k < name->typeArgs.size(); ++k)
      bound[k] = resolveType(name->typeArgs[k], /*allowVoid=*/false);
  } else if (expected) {
    // `lists.map(first)`, `const f: (xs: int[]) => int | undefined = first`
    // From the parameters first; the result gives only what they do not
    // (a result that does not fit is reported where the value goes).
    Inference fromParams(typeParams, aliasOrigins_, nullptr), fromResult(typeParams, aliasOrigins_, nullptr);
    for (size_t i = 0; i < generic->params.size() && i < expected->params.size(); ++i)
      if (expected->params[i])
        fromParams.from(generic->params[i], expected->params[i]);
    if (ret && expected->ret && !expected->voidResult)
      fromResult.from(ret, expected->ret);
    for (size_t k = 0; k < typeParams.size(); ++k) {
      for (const Inference *inference : {&fromParams, &fromResult}) {
        const Type *type = nullptr;
        for (const Type *c : inference->candidates[k]) {
          type = type ? unify(type, c) : c;
          if (!type)
            break;
        }
        if (type && !bound[k])
          bound[k] = type;
      }
    }
  } else {
    diags_.error(name->loc(), "'" + generic->name + "' is generic, so a value of it needs its type arguments, "
                              "e.g. '" + generic->name + "<int>', or a function type where it goes");
    return name;
  }
  if (!finishTypeArgs(generic, bound, name->loc(), /*call=*/false) || !ret)
    return name;
  std::vector<const Type *> params;
  for (const Type *p : generic->params)
    params.push_back(substitute(p, typeParams, bound));
  name->func = generic;
  name->type = Type::function(params, generic->minArgs, generic->hasRest, substitute(ret, typeParams, bound),
                              generic->hasThis);
  bool concrete = llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); });
  if (concrete && !trial_ && !generic->inferring)
    if (FuncSymbol *instance = instantiate(generic, bound, name->loc()))
      name->func = instance;
  return name;
}

bool Sema::returnsOwnClass(const FuncSymbol *generic, const Type *ret) {
  llvm::SmallPtrSet<const Type *, 4> seen;
  const ClassDecl *cls = ret ? literalClassIn(ret, seen) : nullptr;
  if (!cls)
    return false;
  if (ownClassReturns_.insert(generic).second)
    diags_.error(cls->literal->loc(), "generic function '" + generic->name +
                                          "' cannot return an object literal with methods yet: each copy of "
                                          "it makes a class of its own; declare a class, or give the "
                                          "object function fields, 'f: () => ...'");
  return true;
}

//===----------------------------------------------------------------------===//
// Instances
//===----------------------------------------------------------------------===//

std::vector<const Type *> Sema::exactClassArgs(CallExpr *call, const FuncSymbol *generic) {
  std::vector<const Type *> exact;
  if (generic->hasRest)
    return exact;
  for (size_t i = 0; i < generic->params.size() && i < call->args.size(); ++i) {
    const Type *param = generic->params[i];
    if (!param || !param->isConstructor() || !mentionsAnyOf(param, generic->typeParams))
      continue;
    Expr *arg = call->args[i];
    std::vector<const CastExpr *> dropped;
    while (auto *conv = dyn_cast<CastExpr>(arg)) {
      if (!conv->isImplicit())
        break;
      dropped.push_back(conv);
      arg = conv->operand;
    }
    const ClassDecl *cls = arg->type->classRef();
    if (!cls || !cls->type)
      continue;
    exact.resize(generic->params.size(), nullptr);
    exact[i] = arg->type;
    call->args[i] = arg;
    // The class no longer becomes a Constructor here.
    for (const CastExpr *conv : dropped)
      constructorCasts_.erase(conv);
  }
  return exact;
}

FuncSymbol *Sema::instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args, llvm::SMLoc loc,
                              llvm::ArrayRef<const Type *> exact) {
  if (llvm::any_of(args, [](const Type *t) { return t->isError(); }))
    return nullptr;
  std::vector<const Type *> reps;
  for (size_t k = 0; k < args.size(); ++k)
    reps.push_back(exactTypeParam(generic, k) ? args[k] : representation(args[k]));
  // A copy for classes known when compiling is one of its own.
  std::vector<const Type *> keyTypes = reps;
  if (!exact.empty()) {
    keyTypes.push_back(nullptr);
    keyTypes.insert(keyTypes.end(), exact.begin(), exact.end());
  }
  auto key = std::make_pair(static_cast<const FuncSymbol *>(generic), keyTypes);
  if (auto found = instances_.find(key); found != instances_.end()) {
    Instance *instance = found->second;
    // From the body of another instance: that one fails if this one does.
    if (checkingInstance_ && !instance->checked && !instance->checking)
      checkInstance(instance);
    if (checkingInstance_ && instance->failed)
      reportInstance(instance, loc, args);
    return instance->decl->sym;
  }
  unsigned chain = 0;
  for (const Instance *i = checkingInstance_; i; i = i->requester)
    chain += i->generic == generic;
  if (chain >= kMaxInstanceChain) {
    diags_.error(loc, "calling '" + generic->name + "' here needs another copy of it for each copy of the "
                          "functions that lead here, without end: its type arguments keep growing ('" +
                          typeList(args) + "' by now); generic functions that call each other must pass "
                          "their type parameters on unchanged");
    return nullptr;
  }

  // The types of the copy: a computation over them that has no answer
  // (`keyof` of something that is no object) is reported here.
  for (const Type *t : generic->params)
    if (t && substitute(t, generic->typeParams, reps)->isError()) {
      if (!checkingInstance_)
        diags_.error(loc, "cannot call '" + generic->name + "' with " + bindingList(generic->typeParams, args) +
                              ": the types of its parameters, '" + t->name() + "', mean nothing for these types");
      return nullptr;
    }
  Instance &instance = instanceStore_.emplace_back();
  instance.generic = generic;
  instance.args = reps;
  instance.concrete.assign(args.begin(), args.end());
  instance.exact.assign(exact.begin(), exact.end());
  instance.callLoc = loc;
  instance.requester = checkingInstance_;
  instance.decl = instanceDecl(generic, reps, exact);
  instances_[key] = &instance;
  // A copy of a generic method takes `this`: it is emitted with the other
  // methods of its class.
  if (ClassDecl *cls = generic->decl->cls; cls && !generic->decl->isStatic)
    cls->methodInstances.push_back(instance.decl);
  else
    file_->instances.push_back(instance.decl);
  // A copy of a template is checked right away: its body says what it
  // returns.
  if (isTemplate(generic)) {
    checkTemplateInstance(&instance);
    if (instance.failed)
      reportInstance(&instance, loc, args);
    return instance.decl->sym;
  }
  if (checkingInstance_) {
    checkInstance(&instance);
    if (instance.failed)
      reportInstance(&instance, loc, args);
  } else {
    // Checked at the end, when every global is declared, as function bodies
    // are.
    pendingInstances_.push_back(&instance);
  }
  return instance.decl->sym;
}

FunctionDecl *Sema::instanceDecl(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                                 llvm::ArrayRef<const Type *> exact) {
  FunctionDecl *original = generic->decl;
  FunctionDecl *copy = freshCopy(original);
  copy->isInstance = true;
  copy->typeArgs.assign(args.begin(), args.end());
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = generic->name;
  sym->loc = generic->loc;
  sym->decl = copy;
  for (const Type *p : generic->params)
    sym->params.push_back(substitute(p, params, args));
  sym->minArgs = generic->minArgs;
  sym->hasRest = generic->hasRest;
  // A copy of a template finds its own, from its body.
  sym->ret = generic->ret ? substitute(generic->ret, params, args) : nullptr;
  // `x is T` of the instance: of its type arguments.
  sym->predicateParam = generic->predicateParam;
  sym->asserts = generic->asserts;
  if (generic->predicateType)
    sym->predicateType = substitute(generic->predicateType, params, args);
  for (size_t i = 0; i < copy->params.size() && i < original->params.size(); ++i)
    copy->params[i].declaredType = substitute(original->params[i].declaredType, params, args);
  // The classes known when compiling it is made for (see exactClassArgs).
  for (size_t i = 0; i < exact.size() && i < sym->params.size() && i < copy->params.size(); ++i)
    if (exact[i])
      sym->params[i] = copy->params[i].declaredType = exact[i];
  copy->sym = sym;
  // `...args: A` with A an argument list: its parameters.
  expandArgumentList(copy, sym);
  // The copy sees the names of the file that declares the generic function.
  if (Module *home = declModules_.lookup(original))
    declModules_[copy] = home;
  return copy;
}

void Sema::checkInstance(Instance *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  // A generic function with errors in its own body has them in every copy.
  if (!brokenGenerics_.contains(instance->generic)) {
    BodyState saved = enterBody(instance->decl);
    checkingInstance_ = instance;
    diags_.setCapture(&errors);
    flow_ = Flow();
    checkFunctionBody(instance->decl);
    leaveBody(saved);
  }
  instance->checking = false;
  instance->checked = true;
  if (errors.empty())
    return;
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
  if (instance->concrete == instance->args)
    return;
  // The types of the call, which the representations hide (an object is
  // 'object' there), make a clearer message: the same check with them.
  std::vector<Diagnostics::Captured> precise;
  FunctionDecl *copy = instanceDecl(instance->generic, instance->concrete, instance->exact);
  BodyState saved = enterBody(copy);
  checkingInstance_ = instance;
  diags_.setCapture(&precise);
  flow_ = Flow();
  checkFunctionBody(copy);
  leaveBody(saved);
  if (!precise.empty()) {
    instance->message = precise.front().message;
    instance->errorLoc = precise.front().loc;
  }
}

void Sema::reportInstance(Instance *instance, llvm::SMLoc callLoc, llvm::ArrayRef<const Type *> concrete) {
  if (diags_.capture()) {
    // In the body of another instance: this call is where that one fails.
    failedCalls_[callLoc.getPointer()] = instance;
    diags_.error(callLoc, instance->message);
    return;
  }
  const FuncSymbol *generic = instance->generic;
  diags_.error(callLoc, "cannot call '" + generic->name + "' with " + bindingList(generic->typeParams, concrete) +
                            ": " + instance->message);
  // The calls that lead to the error, one instance into another, until
  // they go round.
  // An instance of a generic class is used rather than called.
  auto nameOf = [](const Instance *i) { return i->cls ? i->cls->name : i->generic->name; };
  const Instance *at = instance;
  llvm::SmallPtrSet<const char *, 8> shown;
  while (at && at->errorLoc.isValid() && shown.insert(at->errorLoc.getPointer()).second) {
    const Instance *next = failedCalls_.lookup(at->errorLoc.getPointer());
    if (next)
      diags_.note(at->errorLoc, "in '" + nameOf(at) + "', which " + (next->cls ? "uses '" : "calls '") +
                                    nameOf(next) + "' here");
    else
      diags_.note(at->errorLoc, "in '" + nameOf(at) + "', here");
    at = next;
  }
}

void Sema::checkInstances() {
  for (size_t i = 0; i < pendingInstances_.size(); ++i) {
    Instance *instance = pendingInstances_[i];
    if (instance->checked)
      continue;
    if (instance->cls) {
      checkClassInstance(instance);
      if (instance->failed)
        reportClassInstance(instance, instance->callLoc);
      continue;
    }
    checkInstance(instance);
    if (instance->failed)
      reportInstance(instance, instance->callLoc, instance->concrete);
  }
  pendingInstances_.clear();
}

//===----------------------------------------------------------------------===//
// Generic aliases
//===----------------------------------------------------------------------===//

std::vector<const Type *> Sema::aliasTypeParams(TypeAliasDecl *alias) {
  auto found = aliasParams_.find(alias);
  if (found == aliasParams_.end()) {
    typeScopes_.emplace_back();
    std::vector<const Type *> defaults;
    std::vector<const Type *> params = declareTypeParams(alias->typeParams, &defaults);
    typeScopes_.pop_back();
    found = aliasParams_.try_emplace(alias, std::move(params), std::move(defaults)).first;
  }
  return found->second.first;
}

const Type *Sema::instantiateAlias(TypeAliasDecl *alias, std::vector<const Type *> args, llvm::SMLoc loc) {
  // The instances that tail calls went through: the type is theirs too.
  std::vector<AliasInstance *> chain;
  // Where the program needs the outermost instance, for an endless one.
  if (aliasDepth_ == 0)
    aliasUse_ = loc;
  const Type *type = nullptr;
  for (unsigned calls = 0; !type; ++calls) {
    aliasTypeParams(alias);
    // Copied: instantiating others below may move the map's entries.
    std::vector<const Type *> params = aliasParams_[alias].first,
                              defaults = aliasParams_[alias].second;
    if (args.size() > params.size()) {
      diags_.error(loc, "'" + alias->name + "' takes " + std::to_string(params.size()) +
                            " type argument" + (params.size() == 1 ? "" : "s") + ", but got " +
                            std::to_string(args.size()));
      type = Type::error();
      break;
    }
    for (size_t k = args.size(); k < params.size() && !type; ++k) {
      if (!defaults[k]) {
        diags_.error(loc, "'" + alias->name + "' needs " + std::to_string(params.size()) +
                              " type argument" + (params.size() == 1 ? "" : "s") + ", e.g. '" +
                              alias->name + "<int>'");
        type = Type::error();
        break;
      }
      args.push_back(substitute(defaults[k], params, args));
    }
    for (size_t k = 0; k < params.size() && !type; ++k) {
      if (args[k]->isError()) {
        type = args[k];
        break;
      }
      if (!satisfies(args[k], params[k], params, args)) {
        diags_.error(loc, "type '" + args[k]->name() + "' does not satisfy the constraint '" +
                              constraintSpelling(params[k], &alias->typeParams[k]) + "' of '" +
                              params[k]->name() + "' in '" + alias->name + "'");
        type = Type::error();
      }
    }
    if (type)
      break;

    // A std::map: the entry stays where it is while others are added.
    AliasInstance &entry = aliasInstances_[{alias, args}];
    if (entry.type) {
      type = entry.type;
      break;
    }
    if (entry.resolving) {
      // An object type may refer to itself through its fields.
      if (entry.self) {
        entry.recursive = true;
        type = entry.self;
        break;
      }
      diags_.error(alias->loc(), "type alias '" + alias->name + "' refers to itself");
      type = Type::error();
      break;
    }
    bool conditional = alias->type.kind == TypeRef::Kind::Conditional;
    if (aliasDepth_ >= kMaxAliasDepth || calls > kMaxTailCalls) {
      if (!conditional)
        diags_.error(alias->loc(), "type alias '" + alias->name +
                                       "' refers to itself with other type arguments, "
                                       "so it never ends");
      else if (calls > kMaxTailCalls)
        diags_.error(alias->loc(), "type alias '" + alias->name + "' calls itself more than " +
                                       std::to_string(kMaxTailCalls) +
                                       " times, so it may never end; "
                                       "TypeScript stops there too");
      else
        diags_.error(
            alias->loc(),
            "type alias '" + alias->name + "' nests more than " + std::to_string(kMaxAliasDepth) +
                " instances of itself, so it may never "
                "end; TypeScript stops there too, and a branch that is the alias "
                "itself, '... ? " +
                alias->name + "<...> : ...', may go " + std::to_string(kMaxTailCalls) + " deep");
      if (conditional && aliasUse_.isValid() && aliasUse_ != alias->loc())
        diags_.note(aliasUse_, "the instance needed here");
      type = Type::error();
      break;
    }
    // `Pair<int>` in messages.
    std::string display = alias->name.str() + "<" + typeList(args) + ">";
    entry.resolving = true;
    chain.push_back(&entry);
    ++aliasDepth_;
    // The names it uses mean what they mean in its own file, which may be
    // another one: `interface Source<R> { pull?: (c: Controller<R>) => void }`
    // of a file that imports only Source.
    std::vector<llvm::StringMap<Stmt *>> outerTypes;
    Module *home = declModules_.lookup(alias);
    bool moved = home && home != module_;
    if (moved) {
      outerTypes = std::move(typeScopes_);
      typeScopes_ = {state(home).types};
    }
    typeScopes_.emplace_back();
    bindTypeParams(alias->typeParams, args);
    if (alias->type.kind == TypeRef::Kind::Object) {
      entry.self = Type::declareObject(display);
      // A generic interface, `interface Repo<T> extends Named { ... }`, as
      // resolveAlias makes one.
      std::vector<const Type *> bases;
      std::vector<Overload> overloads;
      std::vector<Field> fields = alias->bases.empty() ? resolveFields(alias->type, &overloads)
                                                       : interfaceFields(alias, bases, overloads);
      if (entry.recursive) {
        entry.self->defineFields(std::move(fields), std::move(overloads));
        type = entry.self;
      } else {
        type = Type::object(std::move(fields), display, std::move(overloads));
      }
      for (const Type *base : bases)
        if (!type->isError())
          addBase(type, base);
    } else {
      tailAllowed_ = conditional;
      type = resolveTypeInner(alias->type);
      tailAllowed_ = false;
    }
    typeScopes_.pop_back();
    if (moved)
      typeScopes_ = std::move(outerTypes);
    --aliasDepth_;
    // The conditional type ended in another instance: that one next.
    if (!type && tailCall_) {
      alias = tailCall_->alias;
      args = std::move(tailCall_->args);
      loc = alias->loc();
      tailCall_.reset();
      continue;
    }
    if (!type)
      type = Type::error();
    if (type->isObject() && !type->isError() && !aliasOrigins_.count(type))
      aliasOrigins_[type] = {alias, args};
  }
  for (AliasInstance *entry : chain) {
    entry->resolving = false;
    entry->type = type;
  }
  return type;
}

} // namespace tz
