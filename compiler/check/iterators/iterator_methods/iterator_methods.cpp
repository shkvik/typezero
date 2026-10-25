#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The helpers that make a stage of a lazy chain.
const llvm::StringLiteral kStages[] = {"map", "filter", "take", "drop", "flatMap"};

} // namespace

IteratorMethods::IteratorMethods(Sema &sema) : sema_(sema) {}

void IteratorMethods::markFused(Expr *expr) const {
  while (auto *call = dyn_cast<CallExpr>(expr)) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (!member || member->optional || call->optional) {
      return;
    }
    // `Iterator.from(x)` of an iterator is the iterator itself.
    if (sema_.callChecker().isGlobalName(member->object, "Iterator") && member->name == "from" &&
        call->args.size() == 1) {
      expr = call->args[0];
      continue;
    }
    if (!llvm::is_contained(kStages, member->name)) {
      return;
    }
    call->fused = true;
    expr = member->object;
  }
}

Expr *IteratorMethods::checkArrayIterator(CallExpr *call, MemberExpr *member) {
  const Type *element = member->object->type->inner();
  llvm::StringRef name = member->name;
  if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
    return call;
  }
  if (name == "keys") {
    call->builtin = Builtin::ArrKeys;
    call->type = Type::iterator(Type::intTy());
  } else if (name == "values") {
    call->builtin = Builtin::ArrValues;
    call->type = Type::iterator(element);
  } else {
    call->builtin = Builtin::ArrEntries;
    call->type = Type::iterator(Type::tuple({Type::intTy(), element}));
  }

  return call;
}

Expr *IteratorMethods::checkIteratorFrom(CallExpr *call, MemberExpr *member) {
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (member->name != "from") {
    sema_.diags_.error(member->loc(), "'Iterator." + member->name +
                                    "' is not supported; of Iterator only 'Iterator.from' is");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
    return call;
  }
  call->args[0] = sema_.dictChecker().iterableOf(sema_.exprChecker().checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  const Type *element = GeneratorChecker::iteratedType(type);
  if (!element) {
    if (!type->isError()) {
      sema_.diags_.error(call->args[0]->loc(),
                   "'Iterator.from' needs an array, a string or an iterator, found '" +
                       type->name() + "'");
    }
    return call;
  }
  call->builtin = Builtin::IterFrom;
  // An iterator is its own: Iterator.from gives it back.
  call->type = type->isIterator() ? type : Type::iterator(element);

  return call;
}

Expr *IteratorMethods::checkArrayFrom(CallExpr *call, MemberExpr *member) {
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (member->name != "from") {
    sema_.diags_.error(member->loc(),
                 "'Array." + member->name +
                     "' is not supported; of Array only 'Array.from' and 'new Array' are");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
    return call;
  }
  markFused(call->args[0]);
  call->args[0] = sema_.dictChecker().iterableOf(sema_.exprChecker().checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  const Type *element = GeneratorChecker::iteratedType(type);
  if (!element) {
    if (!type->isError()) {
      sema_.diags_.error(call->args[0]->loc(),
                   "'Array.from' needs an array, a string or an iterator, found '" + type->name() +
                       "'");
    }
    if (call->args.size() == 2) {
      call->args[1] = sema_.exprChecker().checkExpr(call->args[1]);
    }
    return call;
  }
  if (type->isIterator()) {
    sema_.generatorChecker().checkSendsNothing(type, call->args[0]->loc(), "'Array.from'");
  }
  call->builtin = Builtin::ArrayFrom;
  call->type = Type::array(element);
  if (call->args.size() == 2) {
    // `Array.from(xs, (x, i) => ...)`: each value through the function.
    Sema::Signature sig;
    sig.pad = false;
    sig.params = {element, Type::intTy()};
    call->args[1] = sema_.callbackChecker().checkFunctionArg(call->args[1], sig, 1, "Array.from");
    const Type *callback = call->args[1]->type;
    if (!callback->isFunction()) {
      return call;
    }
    const Type *result = callback->ret();
    if (result->isVoid() || result->isUndefined()) {
      sema_.diags_.error(call->args[1]->loc(), "the callback of 'Array.from' must return a value");
      call->type = Type::error();
      return call;
    }
    call->type = Type::array(result);
  }
  sema_.flowChecker().calledFunction(); // a generator or the callback may change anything

  return call;
}

Expr *IteratorMethods::checkIteratorMethod(CallExpr *call, MemberExpr *member) {
  const Type *iterator = member->object->type;
  const Type *element = iterator->yielded();
  llvm::StringRef name = member->name;
  size_t count = call->args.size();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };

  if (name == "next") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    // The value is undefined when the iterator is done, or what a generator
    // returns then: `T | R | undefined`.
    const Type *returned = iterator->returned();
    const Type *value = returned->isVoid() ? element : Type::unionOf({element, returned});
    if (std::string problem = unionProblem(value); !problem.empty()) {
      sema_.diags_.error(member->loc(),
                   "next() of '" + iterator->name() + "' gives '" + element->name() + "', or '" +
                       returned->name() + "' when it is done, and no union holds both: " + problem +
                       "; iterate it with 'for...of', which ignores what it returns");
      checkArgsLoosely();
      return call;
    }
    if (count == 1) {
      // What a generator that ignores it gets is evaluated and dropped.
      const Type *sent = iterator->sent();
      call->args[0] = sent->isVoid() ? sema_.exprChecker().checkExpr(call->args[0]) : sema_.argumentChecker().checkArg(call->args[0], sent);
    }
    call->builtin = Builtin::IterNext;
    call->type =
        Type::object({{"value", Type::optional(value), false}, {"done", Type::boolTy(), false}},
                     "IteratorResult<" + value->name().str() + ">");
    sema_.flowChecker().calledFunction(); // the generator runs
    return call;
  }
  if (name == "return" || name == "throw") {
    sema_.diags_.error(member->loc(), "'" + name +
                                    "()' of an iterator is not supported yet: it comes with "
                                    "exceptions");
    checkArgsLoosely();
    return call;
  }

  struct Helper {
    llvm::StringLiteral name;
    Builtin builtin;
  };
  static const Helper helpers[] = {
      {"map", Builtin::IterMap},         {"filter", Builtin::IterFilter},
      {"take", Builtin::IterTake},       {"drop", Builtin::IterDrop},
      {"flatMap", Builtin::IterFlatMap}, {"toArray", Builtin::IterToArray},
      {"forEach", Builtin::IterForEach}, {"reduce", Builtin::IterReduce},
      {"some", Builtin::IterSome},       {"every", Builtin::IterEvery},
      {"find", Builtin::IterFind},
  };
  const Helper *helper = nullptr;
  for (const Helper &h : helpers) {
    if (h.name == name) {
      helper = &h;
    }
  }
  if (!helper) {
    std::string message =
        "property '" + name.str() + "' does not exist on type '" + iterator->name().str() + "'";
    if (name == "length") {
      message +=
          "; an iterator does not know how many values it gives: collect them with '[...it]'";
    }
    sema_.diags_.error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  Builtin builtin = helper->builtin;
  // The helpers call next() without a value.
  if (!sema_.generatorChecker().checkSendsNothing(iterator, member->loc(), "'" + name.str() + "'")) {
    checkArgsLoosely();
    return call;
  }

  if (builtin == Builtin::IterTake || builtin == Builtin::IterDrop) {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->args[0] = sema_.memberChecker().checkPosition(call->args[0]);
    call->builtin = builtin;
    call->type = Type::iterator(element);
    return call;
  }
  if (builtin == Builtin::IterToArray) {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    call->builtin = builtin;
    call->type = Type::array(element);
    sema_.flowChecker().calledFunction(); // the iterator runs
    return call;
  }

  bool reduce = builtin == Builtin::IterReduce;
  if (count == 0) {
    sema_.diags_.error(member->loc(), "'" + name + "' needs a function");
    return call;
  }
  if (count > (reduce ? 2u : 1u)) {
    sema_.diags_.error(member->loc(), "expected " + argumentCount(1, reduce ? 2 : 1) + ", but got " +
                                    llvm::Twine(count));
    checkArgsLoosely();
    return call;
  }
  // The callback gets the value and a counter (and first the accumulator
  // for reduce), not an index and an array as for arrays.
  Sema::Signature sig;
  sig.pad = false;
  unsigned primary = 1;
  const Type *accumulator = element;
  switch (builtin) {
  case Builtin::IterForEach:
    sig.voidResult = true;
    break;
  case Builtin::IterFilter:
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind:
    sig.ret = Type::boolTy();
    break;
  case Builtin::IterReduce:
    primary = 2;
    if (count == 2) {
      Expr *&init = call->args[1];
      auto *unary = dyn_cast<UnaryExpr>(init);
      bool literal = isa<IntLitExpr>(init) ||
                     (unary && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand));
      init = sema_.exprChecker().checkExpr(init, literal && GenericChecker::apparentType(element)->isNumeric() ? GenericChecker::apparentType(element)
                                                                           : nullptr);
      accumulator = init->type;
      if (accumulator->isUndefined() || accumulator->isVoid()) {
        sema_.diags_.error(init->loc(),
                     "the initial value of 'reduce' needs a type; declare it in a variable "
                     "first, e.g. 'const start: int | undefined = undefined'");
        accumulator = Type::error();
      }
    }
    sig.ret = accumulator;
    break;
  default:
    break;
  }
  if (accumulator->isError()) {
    call->args[0] = sema_.exprChecker().checkExpr(call->args[0]);
    return call;
  }
  sig.params = reduce ? std::vector<const Type *>{accumulator, element, Type::intTy()}
                      : std::vector<const Type *>{element, Type::intTy()};
  bool isBoolean = sema_.callChecker().isGlobalName(call->args[0], "Boolean");
  call->args[0] = sema_.callbackChecker().checkFunctionArg(call->args[0], sig, primary, name);
  // A stage that is not consumed where it is written keeps its callback.
  if (isIteratorStage(builtin)) {
    if (auto *fe = dyn_cast<FunctionExpr>(call->args[0])) {
      fe->fn->nonEscaping = call->fused;
    }
  }
  call->builtin = builtin;
  if (isIteratorSink(builtin)) {
    sema_.flowChecker().calledFunction(); // the callback may change anything
  }
  const Type *callback = call->args[0]->type;
  if (!callback->isFunction()) {
    return call;
  }
  const Type *result = callback->ret();
  switch (builtin) {
  case Builtin::IterMap:
    if (result->isVoid() || result->isUndefined()) {
      sema_.diags_.error(call->args[0]->loc(), "the callback of 'map' must return a value");
      return call;
    }
    call->type = Type::iterator(result);
    break;
  case Builtin::IterFlatMap: {
    // Each result is iterated in turn. JavaScript rejects a string here.
    const Type *inner = result->isString() ? nullptr : GeneratorChecker::iteratedType(result);
    if (!inner) {
      sema_.diags_.error(call->args[0]->loc(),
                   "the callback of 'flatMap' must return an array or an iterator, "
                   "found '" +
                       result->name() + "'");
      return call;
    }
    if (result->isIterator() && !sema_.generatorChecker().checkSendsNothing(result, call->args[0]->loc(), "'flatMap'")) {
      return call;
    }
    call->type = Type::iterator(inner);
    break;
  }
  case Builtin::IterFilter:
    // `filter(x => x != undefined)` keeps only the values that hold one.
    if (const Type *kept = sema_.predicateChecker().filteredType(call->args[0], element)) {
      call->type = Type::iterator(kept);
    } else {
      call->type = Type::iterator(
          element->isOptional() && (isBoolean || sema_.callbackChecker().keepsDefined(call->args[0])) ? element->inner()
                                                                              : element);
    }
    break;
  case Builtin::IterForEach:
    call->type = Type::voidTy();
    break;
  case Builtin::IterReduce:
    call->type = accumulator;
    if (count == 2 && call->args.size() == 2) {
      call->args[1] = sema_.typeRelations().coerce(call->args[1], accumulator);
    }
    break;
  case Builtin::IterSome:
  case Builtin::IterEvery:
    call->type = Type::boolTy();
    break;
  case Builtin::IterFind:
    call->type = Type::optional(element);
    break;
  default:
    break;
  }

  return call;
}

} // namespace tz
