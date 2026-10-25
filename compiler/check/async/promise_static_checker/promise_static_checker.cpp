#include "compiler/check/async/promise_static_checker/promise_static_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// What `await` of a value of `type` gives: the value of a Promise, or the
// value itself.
const Type *awaited(const Type *type) { return type->isPromise() ? type->promised() : type; }

} // namespace

PromiseStaticChecker::PromiseStaticChecker(Checker &checker) : checker_(checker) {}

// The promises (or values) Promise.all and the like take: an array, or an
// array literal whose elements differ, which then is a tuple. `element`
// gets the type of what each gives, `tuple` the types for a tuple.
bool PromiseStaticChecker::checkPromiseList(Expr *&arg, const Type *wantedElement,
                                            const Type *&element, std::vector<const Type *> &tuple,
                                            llvm::StringRef fn) {
  // A union that may hold a Promise, which JavaScript would wait for.
  auto mayBePromise = [&](const Type *t, llvm::SMLoc loc) {
    if (t->isPromise() || AsyncChecker::awaitedType(t) == t) {
      return false;
    }
    checker_.diags_.error(loc,
                          "'" + fn + "' of values that may be promises ('" + t->name() +
                              "') is not supported yet; await each first, or make each a Promise");

    return true;
  };
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg);
      lit && llvm::none_of(lit->elements,
                           [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; })) {
    if (lit->elements.empty()) {
      if (!wantedElement) {
        checker_.diags_.error(lit->loc(),
                              "'" + fn +
                                  "' of no promises needs the type of their values; "
                                  "give the array a type, e.g. 'const ps: Promise<int>[] = []'");
        return false;
      }
      lit->type = Type::array(Type::promise(wantedElement));
      element = wantedElement;
      return true;
    }
    std::vector<const Type *> types;
    bool same = true;
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = checker_.exprChecker().checkExpr(
          e.expr, wantedElement && Destination::literalShaped(e.expr) ? wantedElement : nullptr);
      if (e.expr->type->isError()) {
        return false;
      }
      if (e.expr->type->isVoid()) {
        checker_.diags_.error(e.expr->loc(),
                              "a value of type 'void' cannot be given to '" + fn + "'");
        return false;
      }
      if (mayBePromise(e.expr->type, e.expr->loc())) {
        return false;
      }
      if (!types.empty() && types.front() != e.expr->type) {
        same = false;
      }
      types.push_back(e.expr->type);
    }
    // One type that holds them all makes an array: Promise.reject() gives a
    // Promise of anything.
    const Type *common = types.front();
    for (const Type *t : types) {
      if (common) {
        common = TypeRelations::unify(common, t);
      }
    }
    if (!same && common && !common->isError()) {
      for (ArrayLitExpr::Element &e : lit->elements) {
        e.expr = checker_.typeRelations().convert(e.expr, common);
      }
      types.assign(types.size(), common);
      same = true;
    }
    if (same) {
      lit->type = Type::array(types.front());
      element = awaited(types.front());
      return true;
    }
    lit->type = Type::tuple(types);
    for (const Type *t : types) {
      tuple.push_back(awaited(t));
    }
    return true;
  }
  arg = checker_.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return false;
  }
  if (type->isArray()) {
    if (mayBePromise(type->inner(), arg->loc())) {
      return false;
    }
    element = awaited(type->inner());
    return true;
  }
  if (type->isTuple()) {
    for (const Field &f : type->fields()) {
      if (mayBePromise(f.type, arg->loc())) {
        return false;
      }
      tuple.push_back(awaited(f.type));
    }
    return true;
  }
  // An iterator or a Set, which JavaScript would take too: an array of it.
  auto *name = dyn_cast<NameExpr>(arg);
  std::string spread = type->isIterator() || type->isDict()
                           ? "; make an array of them: '" + fn.str() + "([..." +
                                 (name ? name->name.str() : std::string("values")) + "])'"
                           : "";
  checker_.diags_.error(arg->loc(), "'" + fn + "' needs an array of promises, found '" +
                                        type->name() + "'" + spread);

  return false;
}

Expr *PromiseStaticChecker::checkPromiseStatic(CallExpr *call, MemberExpr *member,
                                               const Type *expected) {
  llvm::StringRef name = member->name;
  checker_.file_->usesAsync = true;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (!member->typeArgs.empty() && name != "withResolvers") {
    checker_.diags_.error(member->loc(),
                          "type arguments can be given to 'Promise.withResolvers<T>()'; "
                          "the other functions of Promise take their types from what they "
                          "get");
    member->typeArgs.clear();
  }
  const Type *wanted = expected && expected->nonOptional()->isPromise()
                           ? expected->nonOptional()->promised()
                           : nullptr;
  if (wanted && wanted->isNever()) {
    wanted = nullptr;
  }
  std::string fn = "Promise." + name.str();

  if (name == "resolve") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    call->builtin = Builtin::PromiseResolve;
    if (call->args.empty()) {
      call->type = Type::promise(Type::voidTy());
      return call;
    }
    Expr *&arg = call->args[0];
    arg = checker_.exprChecker().checkExpr(arg,
                                           wanted && !isa<UndefinedExpr>(arg) ? wanted : nullptr);
    const Type *type = arg->type;
    if (type->isError()) {
      return call;
    }
    if (type->isPromise()) { // that Promise itself
      call->type = type;
      return call;
    }
    if (type->isUndefined()) {
      call->type = Type::promise(Type::voidTy());
      return call;
    }
    if (type->isVoid()) {
      checker_.diags_.error(arg->loc(),
                            "a value of type 'void' cannot be given to 'Promise.resolve'");
      return call;
    }
    call->type = Type::promise(type);
    return call;
  }
  if (name == "reject") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->builtin = Builtin::PromiseReject;
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], checker_.errorClass_->type);
    call->type = Type::promise(wanted ? wanted : Type::never());
    return call;
  }
  if (name == "withResolvers") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    call->builtin = Builtin::PromiseWithResolvers;
    const Type *value = nullptr;
    if (member->typeArgs.size() > 1) {
      checker_.diags_.error(member->loc(),
                            "'Promise.withResolvers' takes one type argument, the type of "
                            "the value");
      return call;
    }
    if (member->typeArgs.size() == 1) {
      value = awaited(checker_.typeResolver().resolveType(member->typeArgs[0], /*allowVoid=*/true));
    } else if (expected && expected->isObject()) {
      if (const Field *promise = expected->field("promise");
          promise && promise->type->isPromise()) {
        value = promise->type->promised();
      }
    }
    member->typeArgs.clear();
    if (!value) {
      checker_.diags_.error(member->loc(),
                            "'Promise.withResolvers' needs the type of the value; write it, "
                            "e.g. 'Promise.withResolvers<int>()'");
      return call;
    }
    if (value->isError()) {
      return call;
    }
    call->type = Type::object({{"promise", Type::promise(value), false},
                               {"resolve", PromiseChecker::resolverType(value), false},
                               {"reject", checker_.promiseChecker().rejecterType(), false}});
    return call;
  }
  static const std::pair<llvm::StringLiteral, Builtin> lists[] = {
      {"all", Builtin::PromiseAll},
      {"allSettled", Builtin::PromiseAllSettled},
      {"race", Builtin::PromiseRace},
      {"any", Builtin::PromiseAny}};
  for (const auto &[listName, builtin] : lists) {
    if (name != listName) {
      continue;
    }
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->builtin = builtin;
    const Type *wantedElement = nullptr;
    if (wanted && builtin == Builtin::PromiseAll && wanted->isArray()) {
      wantedElement = wanted->inner();
    } else if (wanted && (builtin == Builtin::PromiseRace || builtin == Builtin::PromiseAny)) {
      wantedElement = wanted;
    }
    const Type *element = nullptr;
    std::vector<const Type *> tuple;
    if (!checkPromiseList(call->args[0], wantedElement, element, tuple, fn)) {
      return call;
    }
    if (!tuple.empty() && (builtin == Builtin::PromiseRace || builtin == Builtin::PromiseAny)) {
      // One value comes out of race and any: of one type, or of their union,
      // as in TypeScript.
      element = tuple.front();
      for (const Type *t : tuple) {
        const Type *common = LiteralTypes::unionUnify(element, t);
        if (!common || common->isError()) {
          checker_.diags_.error(call->args[0]->loc(), "'" + fn +
                                                          "' gives a value of one of the promises, "
                                                          "but they give '" +
                                                          element->name() + "' and '" + t->name() +
                                                          "'; make them give one type");
          return call;
        }
        element = common;
      }
    }
    if (element && element->isNever()) {
      element = wantedElement ? wantedElement : Type::voidTy();
    }
    switch (builtin) {
    case Builtin::PromiseAll:
      if (!tuple.empty()) {
        for (const Type *t : tuple) {
          if (t->isVoid()) {
            checker_.diags_.error(call->args[0]->loc(),
                                  "'Promise.all' collects the values, and a "
                                  "Promise<void> has none; await it on its own");
            return call;
          }
        }
        call->type = Type::promise(Type::tuple(tuple));
      } else if (element->isVoid()) {
        // Nothing to collect (JavaScript gives an array of undefined, which
        // tz has no type for): it only waits.
        call->type = Type::promise(Type::voidTy());
      } else {
        call->type = Type::promise(Type::array(element));
      }
      break;
    case Builtin::PromiseAllSettled:
      if (!tuple.empty()) {
        // A tuple of results, each of its own type, as in TypeScript.
        std::vector<const Type *> results;
        for (const Type *t : tuple) {
          results.push_back(
              checker_.promiseChecker().settledResultType(t->isNever() ? Type::voidTy() : t));
        }
        call->type = Type::promise(Type::tuple(results));
      } else {
        call->type =
            Type::promise(Type::array(checker_.promiseChecker().settledResultType(element)));
      }
      break;
    default:
      call->type = Type::promise(element);
      break;
    }
    return call;
  }
  checker_.diags_.error(member->loc(),
                        "'Promise." + name +
                            "' is not supported; the functions of Promise "
                            "are 'resolve', 'reject', 'all', 'allSettled', 'race', 'any' and "
                            "'withResolvers'");
  checkArgsLoosely();

  return call;
}

} // namespace tz
