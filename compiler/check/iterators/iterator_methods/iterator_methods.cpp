#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/functions/callback_checker/callback_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

const llvm::StringLiteral kStages[] = {"map", "filter", "take", "drop", "flatMap"};

}

IteratorMethods::IteratorMethods(Checker &owner)
    : checker(owner) {}

void IteratorMethods::markFused(Expr *expr) const {
  while (auto *call = dyn_cast<CallExpr>(expr)) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (!member || member->optional || call->optional) {
      return;
    }
    if (this->checker.callChecker().isGlobalName(member->object, "Iterator") &&
        member->name == "from" && call->args.size() == 1) {
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

IteratorMethods::ResultArrayIterateCall IteratorMethods::checkArrayIterator(CallExpr *call,
                                                                            MemberExpr *member) {
  const Type *element = member->object->type->inner();
  llvm::StringRef name = member->name;
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(IteratorMethodsCheckArrayIteratorError{
        .code = IteratorMethodsCheckArrayIteratorErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (name == "keys") {
    call->target = BuiltinCall{.builtin = Builtin::ArrKeys};
    call->type = Type::iterator(Type::intTy());
  } else if (name == "values") {
    call->target = BuiltinCall{.builtin = Builtin::ArrValues};
    call->type = Type::iterator(element);
  } else {
    call->target = BuiltinCall{.builtin = Builtin::ArrEntries};
    call->type = Type::iterator(Type::tuple({Type::intTy(), element}));
  }

  return ok(call);
}

IteratorMethods::ResultSourceIsIterable IteratorMethods::iteratedSource(const Type *type,
                                                                        llvm::StringRef fn) {
  const Type *element = GeneratorChecker::iteratedType(type);
  if (!element && type->isError()) {
    return err(IteratorMethodsIteratedSourceError{
        .code = IteratorMethodsIteratedSourceErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!element) {
    std::string message = ErrorTextIterSource::notIter(fn, type->name());

    return err(IteratorMethodsIteratedSourceError{
        .code = IteratorMethodsIteratedSourceErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(element);
}

IteratorMethods::ResultIteratorFromCall IteratorMethods::checkIteratorFrom(CallExpr *call,
                                                                           MemberExpr *member) {
  if (this->checker.callChecker().rejectSpread(call)) {
    return err(IteratorMethodsCheckIteratorFromError{
        .code = IteratorMethodsCheckIteratorFromErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (member->name != "from") {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextStaticFrom::iterator(member->name);

    return err(IteratorMethodsCheckIteratorFromError{
        .code = IteratorMethodsCheckIteratorFromErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(IteratorMethodsCheckIteratorFromError{
        .code = IteratorMethodsCheckIteratorFromErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  call->args[0] =
      this->checker.dictChecker().iterableOf(this->checker.exprChecker().checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  ResultSourceIsIterable source = IteratorMethods::iteratedSource(type, "Iterator.from");
  if (source.isErr()) {
    bool reported = source.error().code == IteratorMethodsIteratedSourceErrorCode::ALREADY_REPORTED;

    return err(IteratorMethodsCheckIteratorFromError{
        .code = reported ? IteratorMethodsCheckIteratorFromErrorCode::ALREADY_REPORTED
                         : IteratorMethodsCheckIteratorFromErrorCode::NOT_ITERABLE,
        .message = source.error().message,
    });
  }
  call->target = BuiltinCall{.builtin = Builtin::IterFrom};
  call->type = type->isIterator() ? type : Type::iterator(source.value());

  return ok(call);
}

IteratorMethods::ResultArrayStaticKnown IteratorMethods::arrayStatic(const MemberExpr *member) {
  if (member->name != "from") {
    std::string message = ErrorTextStaticFrom::array(member->name);

    return err(IteratorMethodsArrayStaticError{
        .code = IteratorMethodsArrayStaticErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(member);
}

IteratorMethods::ResultCallbackHasValue IteratorMethods::callbackValue(const Type *result,
                                                                       llvm::StringRef fn) {
  if (result->isVoid() || result->isUndefined()) {
    std::string message = ErrorTextCallbackValue::missing(fn);

    return err(IteratorMethodsCallbackValueError{
        .code = IteratorMethodsCallbackValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(result);
}

Expr *IteratorMethods::checkArrayFrom(CallExpr *call, MemberExpr *member) {
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  ResultArrayStaticKnown known = IteratorMethods::arrayStatic(member);
  if (known.isErr()) {
    this->checker.diags_.error(member->loc(), known.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
    return call;
  }
  this->markFused(call->args[0]);
  call->args[0] =
      this->checker.dictChecker().iterableOf(this->checker.exprChecker().checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  ResultSourceIsIterable source = IteratorMethods::iteratedSource(type, "Array.from");
  if (source.isErr()) {
    if (source.error().code != IteratorMethodsIteratedSourceErrorCode::ALREADY_REPORTED) {
      this->checker.diags_.error(call->args[0]->loc(), source.error().message);
    }
    if (call->args.size() == 2) {
      call->args[1] = this->checker.exprChecker().checkExpr(call->args[1]);
    }
    return call;
  }
  const Type *element = source.value();
  if (type->isIterator()) {
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        this->checker.generatorChecker().checkSendsNothing(type, "'Array.from'");
    if (nothing.isErr()) {
      this->checker.diags_.error(call->args[0]->loc(), nothing.error().message);
    }
  }
  call->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  call->type = Type::array(element);
  if (call->args.size() == 2) {
    Checker::Signature sig;
    sig.pad = false;
    sig.params = {element, Type::intTy()};
    call->args[1] =
        this->checker.callbackChecker().checkFunctionArg(call->args[1], sig, 1, "Array.from");
    const Type *callback = call->args[1]->type;
    if (!callback->isFunction()) {
      return call;
    }
    ResultCallbackHasValue value = IteratorMethods::callbackValue(callback->ret(), "Array.from");
    if (value.isErr()) {
      this->checker.diags_.error(call->args[1]->loc(), value.error().message);
      call->type = Type::error();
      return call;
    }
    call->type = Type::array(value.value());
  }
  this->checker.flowChecker().calledFunction();

  return call;
}

IteratorMethods::ResultNextValueOneType IteratorMethods::nextValue(const Type *iterator) {
  const Type *element = iterator->yielded();
  const Type *returned = iterator->returned();
  const Type *value = returned->isVoid() ? element : Type::unionOf({element, returned});
  std::string problem = unionProblem(value);
  if (!problem.empty()) {
    std::string message =
        ErrorTextNextValue::noUnion(iterator->name(), element->name(), returned->name(), problem);

    return err(IteratorMethodsNextValueError{
        .code = IteratorMethodsNextValueErrorCode::UNION_PROBLEM,
        .message = message,
    });
  }

  return ok(value);
}

IteratorMethods::ResultNotThrowOrReturn IteratorMethods::plainMethod(llvm::StringRef name) {
  if (name == "throw") {
    std::string message = ErrorTextIteratorMethod::noThrow();

    return err(IteratorMethodsPlainMethodError{
        .code = IteratorMethodsPlainMethodErrorCode::NO_THROW,
        .message = message,
    });
  }
  if (name == "return") {
    std::string message = ErrorTextIteratorMethod::noReturn();

    return err(IteratorMethodsPlainMethodError{
        .code = IteratorMethodsPlainMethodErrorCode::NO_RETURN,
        .message = message,
    });
  }

  return ok(name);
}

IteratorMethods::ResultKnownHelperFound IteratorMethods::iteratorHelper(llvm::StringRef name,
                                                                        const Type *iterator) {
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
    std::string message = ErrorTextIteratorMethod::noProperty(name, iterator->name());

    return err(IteratorMethodsIteratorHelperError{
        .code = IteratorMethodsIteratorHelperErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(helper);
}

IteratorMethods::ResultCallbackIsPassed
IteratorMethods::callbackGiven(size_t count, llvm::StringRef name, bool reduce) {
  if (count == 0) {
    std::string message = ErrorTextIteratorMethod::noFunction(name);

    return err(IteratorMethodsCallbackGivenError{
        .code = IteratorMethodsCallbackGivenErrorCode::NO_FUNCTION,
        .message = message,
    });
  }
  unsigned most = reduce ? 2u : 1u;
  if (count > most) {
    std::string message = ErrorTextArity::count(1, most, count);

    return err(IteratorMethodsCallbackGivenError{
        .code = IteratorMethodsCallbackGivenErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(count);
}

IteratorMethods::ResultInitialValueType IteratorMethods::initialValue(const Type *accumulator) {
  if (accumulator->isUndefined() || accumulator->isVoid()) {
    std::string message = ErrorTextReduceInit::untyped();

    return err(IteratorMethodsInitialValueError{
        .code = IteratorMethodsInitialValueErrorCode::UNTYPED_INITIAL,
        .message = message,
    });
  }

  return ok(accumulator);
}

IteratorMethods::ResultFlatMapInnerType IteratorMethods::flatMapInner(const Type *result) {
  const Type *inner = result->isString() ? nullptr : GeneratorChecker::iteratedType(result);
  if (!inner) {
    std::string message = ErrorTextFlatMap::notIter(result->name());

    return err(IteratorMethodsFlatMapInnerError{
        .code = IteratorMethodsFlatMapInnerErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(inner);
}

Expr *IteratorMethods::checkIteratorMethod(CallExpr *call, MemberExpr *member) {
  const Type *iterator = member->object->type;
  const Type *element = iterator->yielded();
  llvm::StringRef name = member->name;
  size_t count = call->args.size();

  if (name == "next") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    ResultNextValueOneType next = IteratorMethods::nextValue(iterator);
    if (next.isErr()) {
      this->checker.diags_.error(member->loc(), next.error().message);
      this->checker.argumentChecker().checkArgsLoosely(call);
      return call;
    }
    const Type *value = next.value();
    if (count == 1) {
      const Type *sent = iterator->sent();
      call->args[0] = sent->isVoid()
                          ? this->checker.exprChecker().checkExpr(call->args[0])
                          : this->checker.argumentChecker().checkArg(call->args[0], sent);
    }
    call->target = BuiltinCall{.builtin = Builtin::IterNext};
    call->type =
        Type::object({{"value", Type::optional(value), false}, {"done", Type::boolTy(), false}},
                     "IteratorResult<" + value->name().str() + ">");
    this->checker.flowChecker().calledFunction();
    return call;
  }
  ResultNotThrowOrReturn plain = IteratorMethods::plainMethod(name);
  if (plain.isErr()) {
    this->checker.diags_.error(member->loc(), plain.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  ResultKnownHelperFound found = IteratorMethods::iteratorHelper(name, iterator);
  if (found.isErr()) {
    this->checker.diags_.error(member->loc(), found.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  Builtin builtin = found.value()->builtin;
  std::string what = quotedName(name);
  GeneratorChecker::ResultNextNeedsNoValue nothing =
      this->checker.generatorChecker().checkSendsNothing(iterator, what);
  if (nothing.isErr()) {
    this->checker.diags_.error(member->loc(), nothing.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }

  if (builtin == Builtin::IterTake || builtin == Builtin::IterDrop) {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    call->args[0] = this->checker.memberChecker().checkPosition(call->args[0]);
    std::string counted = ErrorTextIteratorMethod::countOf(name);
    IndexProofs::ResultBetweenProven between =
        this->checker.indexProofs().requireBetween(call->args[0], 0, std::nullopt, counted);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->checker.diags_.error(call->args[0]->loc(), between.error().message);
    }
    call->target = BuiltinCall{.builtin = builtin};
    call->type = Type::iterator(element);
    return call;
  }
  if (builtin == Builtin::IterToArray) {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    call->target = BuiltinCall{.builtin = builtin};
    call->type = Type::array(element);
    this->checker.flowChecker().calledFunction();
    return call;
  }

  bool reduce = builtin == Builtin::IterReduce;
  ResultCallbackIsPassed passed = IteratorMethods::callbackGiven(count, name, reduce);
  if (passed.isErr()) {
    this->checker.diags_.error(member->loc(), passed.error().message);
    this->checker.argumentChecker().checkArgsLoosely(call);
    return call;
  }
  Checker::Signature sig;
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
      init = this->checker.exprChecker().checkExpr(
          init, literal && GenericChecker::apparentType(element)->isNumeric()
                    ? GenericChecker::apparentType(element)
                    : nullptr);
      accumulator = init->type;
      ResultInitialValueType initial = IteratorMethods::initialValue(accumulator);
      if (initial.isErr()) {
        this->checker.diags_.error(init->loc(), initial.error().message);
        accumulator = Type::error();
      }
    }
    sig.ret = accumulator;
    break;
  default:
    break;
  }
  if (accumulator->isError()) {
    call->args[0] = this->checker.exprChecker().checkExpr(call->args[0]);
    return call;
  }
  sig.params = reduce ? std::vector<const Type *>{accumulator, element, Type::intTy()}
                      : std::vector<const Type *>{element, Type::intTy()};
  bool isBoolean = this->checker.callChecker().isGlobalName(call->args[0], "Boolean");
  if (auto *fe = dyn_cast<FunctionExpr>(call->args[0])) {
    this->checker.indexCallbacks_[fe->fn] = reduce ? 2 : 1;
  }
  call->args[0] =
      this->checker.callbackChecker().checkFunctionArg(call->args[0], sig, primary, name);
  if (isIteratorStage(builtin)) {
    if (auto *fe = dyn_cast<FunctionExpr>(call->args[0])) {
      fe->fn->nonEscaping = call->fused;
    }
  }
  call->target = BuiltinCall{.builtin = builtin};
  if (isIteratorSink(builtin)) {
    this->checker.flowChecker().calledFunction();
  }
  const Type *callback = call->args[0]->type;
  if (!callback->isFunction()) {
    return call;
  }
  const Type *result = callback->ret();
  switch (builtin) {
    case Builtin::IterMap: {
      ResultCallbackHasValue value = IteratorMethods::callbackValue(result, "map");
      if (value.isErr()) {
        this->checker.diags_.error(call->args[0]->loc(), value.error().message);
        return call;
      }
      call->type = Type::iterator(result);
      break;
    }
  case Builtin::IterFlatMap: {
    ResultFlatMapInnerType inner = IteratorMethods::flatMapInner(result);
    if (inner.isErr()) {
      this->checker.diags_.error(call->args[0]->loc(), inner.error().message);
      return call;
    }
    if (result->isIterator()) {
      GeneratorChecker::ResultNextNeedsNoValue flatSends =
          this->checker.generatorChecker().checkSendsNothing(result, "'flatMap'");
      if (flatSends.isErr()) {
        this->checker.diags_.error(call->args[0]->loc(), flatSends.error().message);
        return call;
      }
    }
    call->type = Type::iterator(inner.value());
    break;
  }
  case Builtin::IterFilter:
    if (const Type *kept = this->checker.predicateChecker().filteredType(call->args[0], element)) {
      call->type = Type::iterator(kept);
    } else {
      call->type = Type::iterator(
          element->isOptional() &&
                  (isBoolean || this->checker.callbackChecker().keepsDefined(call->args[0]))
              ? element->inner()
              : element);
    }
    break;
  case Builtin::IterForEach:
    call->type = Type::voidTy();
    break;
  case Builtin::IterReduce:
    call->type = count == 2 ? accumulator : Type::optional(accumulator);
    if (count == 2 && call->args.size() == 2) {
      call->args[1] = this->checker.typeRelations().coerce(call->args[1], accumulator);
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

}
