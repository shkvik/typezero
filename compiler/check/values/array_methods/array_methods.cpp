#include "compiler/check/values/array_methods/array_methods.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// The array variable a method is called on, if it is one.
const VarSymbol *arrayVariable(const Expr *object) {
  if (auto *conv = dyn_cast<CastExpr>(object);
      conv && conv->isImplicit() && conv->operand->type->isOptional()) {
    object = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(object);
  if (!name || !name->var || !name->var->type->nonOptional()->isArray()) {
    return nullptr;
  }

  return name->var;
}

} // namespace

std::string argumentCount(unsigned min, unsigned max) {
  std::string text = std::to_string(min);
  if (max != min) {
    text += max == ~0u ? " or more" : "-" + std::to_string(max);
  }

  return text + (max == 1 && min == 1 ? " argument" : " arguments");
}

ArrayMethods::ArrayMethods(Checker &checker) : checker_(checker) {}

Expr *ArrayMethods::checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  const VarSymbol *self = arrayVariable(member->object);
  llvm::StringRef name = member->name;
  size_t count = call->args.size();

  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  auto arity = [&](unsigned min, unsigned max) {
    if (count >= min && count <= max) {
      return true;
    }
    checker_.diags_.error(member->loc(), "expected " + argumentCount(min, max) + ", but got " +
                                             llvm::Twine(count));
    checkArgsLoosely();

    return false;
  };
  auto elementArg = [&](size_t i) {
    call->args[i] = checker_.typeRelations().coerce(
        checker_.exprChecker().checkExpr(call->args[i], element), element);
  };
  auto positionArgs = [&](size_t from) {
    for (size_t i = from; i < count; ++i) {
      call->args[i] = checker_.memberChecker().checkPosition(call->args[i]);
    }
  };

  static const char *const callbacks[] = {"map",         "filter",  "forEach",   "reduce",
                                          "reduceRight", "find",    "findIndex", "some",
                                          "every",       "flatMap", "findLast",  "findLastIndex"};
  if (llvm::is_contained(callbacks, name) || (name == "sort" && count != 0)) {
    return checker_.callbackChecker().checkCallbackMethod(call, member, expected);
  }
  if (name == "keys" || name == "values" || name == "entries") {
    return checker_.iteratorMethods().checkArrayIterator(call, member);
  }

  if (name == "push" || name == "unshift") {
    bool spreads = call->hasSpread();
    for (size_t i = 0; i < count; ++i) {
      if (call->spreads(i)) { // `a.push(...b)`: b has the same type as a
        call->args[i] = checker_.typeRelations().coerce(
            checker_.exprChecker().checkExpr(call->args[i], arrayType), arrayType);
      } else {
        elementArg(i);
      }
    }
    call->builtin = name == "push" ? Builtin::ArrPush : Builtin::ArrUnshift;
    call->type = Type::intTy();
    // The length grows by exactly `count`, unless arrays are spread.
    if (self && !spreads) {
      checker_.flow_.facts.shift(Term::lengthOf(self), static_cast<int64_t>(count));
    }
    checker_.flowChecker().arrayResized(arrayType, /*grew=*/true, /*shrank=*/false,
                                        spreads ? nullptr : self);
    return call;
  }

  if (name == "pop" || name == "shift") {
    if (!arity(0, 0)) {
      return call;
    }
    call->builtin = name == "pop" ? Builtin::ArrPop : Builtin::ArrShift;
    bool notEmpty = checker_.indexProofs().provesNotEmpty(member->object);
    call->mayBeOutOfRange = !notEmpty;
    call->type = notEmpty ? element : Type::optional(element);
    if (self) {
      if (notEmpty) {
        checker_.flow_.facts.shift(Term::lengthOf(self), -1);
      } else {
        checker_.flow_.facts.forgetIf(
            [&](const Facts::Fact &f) { return f.rhs == Term::lengthOf(self); });
      }
    }
    checker_.flowChecker().arrayResized(arrayType, /*grew=*/false, /*shrank=*/true, self);
    return call;
  }

  if (name == "at") {
    if (!arity(1, 1)) {
      return call;
    }
    positionArgs(0);
    call->builtin = Builtin::ArrAt;
    call->mayBeOutOfRange = !checker_.indexProofs().provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(element) : element;
    return call;
  }

  if (name == "slice") {
    if (!arity(0, 2)) {
      return call;
    }
    positionArgs(0);
    call->builtin = Builtin::ArrSlice;
    call->type = arrayType;
    return call;
  }

  if (name == "concat") {
    // Each argument is an array of the same type or a single element.
    for (size_t i = 0; i < count; ++i) {
      call->args[i] = checker_.exprChecker().checkExpr(call->args[i], arrayType);
      if (call->args[i]->type != arrayType) {
        call->args[i] = checker_.typeRelations().coerce(call->args[i], element);
      }
    }
    call->builtin = Builtin::ArrConcat;
    call->type = arrayType;
    return call;
  }

  if (name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    if (!arity(1, 2)) {
      return call;
    }
    if (Checker::holdsResults(element)) {
      checker_.diags_.error(member->loc(),
                            "'" + name +
                                "' cannot compare Results: in JavaScript each Result is an "
                                "object of its own, equal to no other; search with a "
                                "function, e.g. 'some' or 'findIndex'");
      checkArgsLoosely();
      return call;
    }
    if (element->nonOptional()->isFunction()) {
      checker_.file_->comparesFunctions = true;
    }
    elementArg(0);
    positionArgs(1);
    call->builtin = name == "indexOf"       ? Builtin::ArrIndexOf
                    : name == "lastIndexOf" ? Builtin::ArrLastIndexOf
                                            : Builtin::ArrIncludes;
    call->type = name == "includes" ? Type::boolTy() : Type::intTy();
    return call;
  }

  if ((name == "join" || name == "toString") && Checker::holdsFunctions(element)) {
    checker_.diags_.error(member->loc(), "functions cannot be converted to strings");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && Checker::holdsResults(element)) {
    checker_.diags_.error(member->loc(),
                          "Results cannot be converted to strings: JavaScript would give "
                          "'[object Object]'; make the text of each first, e.g. 'a.map(r => "
                          "r.match(...))." +
                              name + "()'");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && UnknownChecker::holdsUnknown(element)) {
    checker_.diags_.error(member->loc(),
                          "unknown values cannot be converted to strings: one may hold an "
                          "object; map them to text first, e.g. 'a.map(x => typeof x == "
                          "\"string\" ? x : \"?\")." +
                              name + "()'");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && JsonChecker::holdsJson(element)) {
    checker_.diags_.error(member->loc(),
                          "Json values cannot be converted to strings: an object would be "
                          "'[object Object]'; map them to text first, e.g. 'a.map(x => "
                          "JSON.stringify(x))." +
                              name + "()'");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && Checker::holdsObjects(element) &&
      !Checker::holdsDates(element)) {
    checker_.diags_.error(member->loc(),
                          "objects cannot be converted to strings: JavaScript would give "
                          "'[object Object]'; map them to text first, e.g. 'a.map(o => o.name)." +
                              name + "()'");
    checkArgsLoosely();
    return call;
  }
  if (name == "join") {
    if (!arity(0, 1)) {
      return call;
    }
    if (count == 1) {
      call->args[0] = checker_.typeRelations().coerce(
          checker_.exprChecker().checkExpr(call->args[0], Type::string()), Type::string());
    }
    call->builtin = Builtin::ArrJoin;
    call->type = Type::string();
    return call;
  }

  if (name == "reverse" || name == "toString") {
    if (!arity(0, 0)) {
      return call;
    }
    call->builtin = name == "reverse" ? Builtin::ArrReverse : Builtin::ArrToString;
    call->type = name == "reverse" ? arrayType : Type::string();
    return call;
  }

  if (name == "fill") {
    if (!arity(1, 3)) {
      return call;
    }
    elementArg(0);
    positionArgs(1);
    call->builtin = Builtin::ArrFill;
    call->type = arrayType;
    return call;
  }

  if (name == "sort") {
    if (Checker::holdsFunctions(element)) {
      checker_.diags_.error(member->loc(),
                            "sorting functions needs a comparison function: 'sort()' "
                            "without one compares their source code");
      return call;
    }
    if (UnknownChecker::holdsUnknown(element)) {
      checker_.diags_.error(member->loc(),
                            "sorting unknown values needs a comparison function: 'sort()' "
                            "without one compares them as strings, objects as '[object "
                            "Object]'; pass one, e.g. 'sort((a, b) => ...)'");
      return call;
    }
    if (JsonChecker::holdsJson(element)) {
      checker_.diags_.error(member->loc(),
                            "sorting Json values needs a comparison function: 'sort()' "
                            "without one compares them as strings, objects as '[object "
                            "Object]'; pass one, e.g. 'sort((a, b) => ...)'");
      return call;
    }
    if (Checker::holdsDates(element)) {
      checker_.diags_.error(member->loc(),
                            "sorting dates needs a comparison function: 'sort()' without one "
                            "compares the texts of toString(), 'Thu Feb 29 2024 ...', which "
                            "puts them in the order of the names of the days; pass one: "
                            "'sort((a, b) => a.getTime() - b.getTime())'");
      return call;
    }
    if (Checker::holdsObjects(element) || Checker::holdsResults(element)) {
      checker_.diags_.error(member->loc(),
                            "sorting objects needs a comparison function: 'sort()' without one "
                            "compares them all as '[object Object]'; pass one, e.g. 'sort((a, b) "
                            "=> a.x - b.x)'");
      return call;
    }
    // A union compares its values as strings too, numbers and objects among
    // them: the same traps.
    auto textual = [](const Type *m) {
      const Type *b = m->isLiteral() ? m->literalBase() : m;

      return b->isString() || b->isBool();
    };
    if (const Type *u = element->nonOptional();
        u->isUnion() && !llvm::all_of(u->members(), textual)) {
      checker_.diags_.error(member->loc(),
                            "'sort()' without a comparison function compares the values "
                            "of '" +
                                u->name() +
                                "' as strings, numbers and objects among "
                                "them; pass one, e.g. 'sort((a, b) => ...)'");
      return call;
    }
    // JavaScript would sort numbers as strings; sorting them differently
    // would give another order than node for the same code. (Undefined
    // elements go to the end without being compared.)
    if (element->nonOptional()->isNumeric() || element->nonOptional()->isDecimal() ||
        GenericChecker::apparentType(element->nonOptional())->isNumeric()) {
      checker_.diags_.error(member->loc(),
                            llvm::Twine("'sort()' without a comparison function compares "
                                        "numbers as strings, as in JavaScript: [2, 10, 1] "
                                        "becomes [1, 10, 2]; pass one: 'sort((x, y) => ") +
                                (element->isOptional() ? "(x ?? 0) - (y ?? 0)" : "x - y") + ")'");
      return call;
    }
    call->builtin = Builtin::ArrSort;
    call->type = arrayType;
    return call;
  }

  if (name == "splice") {
    // splice(start, deleteCount, ...items): removes and returns elements,
    // and inserts items in their place.
    if (!arity(1, ~0u)) {
      return call;
    }
    for (size_t i = 0; i < count; ++i) {
      if (i < 2) {
        call->args[i] = checker_.memberChecker().checkPosition(call->args[i]);
      } else {
        elementArg(i);
      }
    }
    call->builtin = Builtin::ArrSplice;
    call->type = arrayType;
    checker_.flowChecker().arrayResized(arrayType, /*grew=*/true, /*shrank=*/true);
    return call;
  }
  checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                           arrayType->name() + "'");
  checkArgsLoosely();

  return call;
}

} // namespace tz
