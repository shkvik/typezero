#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

IteratorMethods::IteratorMethods(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->memberChecker = &all.memberChecker;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->argumentChecker = &all.argumentChecker;
  this->callbackChecker = &all.callbackChecker;
  this->generatorChecker = &all.generatorChecker;
  this->predicateChecker = &all.predicateChecker;
}

void IteratorMethods::markFused(Expr *expr) const {
  while (auto *call = dyn_cast<CallExpr>(expr)) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (member == nullptr || member->optional || call->optional) {
      return;
    }
    if (this->callChecker->isGlobalMember(member, GlobalMember::ITERATOR_FROM) &&
        call->args.size() == 1) {
      expr = call->args[0];
      continue;
    }
    const auto *stage = IteratorMethodTable::find(member->name);
    if (stage == nullptr || !stage->form.is<IteratorStageMethod>()) {
      return;
    }
    call->fused = true;
    expr = member->object;
  }
}

IteratorMethods::ResultArrayIterateCall IteratorMethods::checkArrayIterator(CallExpr *call,
                                                                            MemberExpr *member) {
  const Type *element = member->object->type->inner();
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(IteratorMethodsCheckArrayIteratorError{
        .code = IteratorMethodsCheckArrayIteratorErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  const auto *method = ArrayMethodTable::find(member->name);
  bool hasKeys = method != nullptr && method->method == ArrayMethod::KEYS;
  bool hasValues = method != nullptr && method->method == ArrayMethod::VALUES;
  if (hasKeys) {
    call->target = BuiltinCall{.builtin = Builtin::ArrKeys};
    call->type = Type::iterator(Type::intTy());
  } else if (hasValues) {
    call->target = BuiltinCall{.builtin = Builtin::ArrValues};
    call->type = Type::iterator(element);
  } else {
    call->target = BuiltinCall{.builtin = Builtin::ArrEntries};
    call->type = Type::iterator(Type::tuple({Type::intTy(), element}));
  }

  return ok(call);
}

IteratorMethods::ResultSourceIsIterable
IteratorMethods::iteratedSource(const Type *type, llvm::StringRef functionName) const {
  const auto *element = this->generatorChecker->iteratedType(type);
  if (element == nullptr && type->isError()) {
    return err(IteratorMethodsIteratedSourceError{
        .code = IteratorMethodsIteratedSourceErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (element == nullptr) {
    auto message = ErrorTextIterSource::notIter(functionName, type->name());

    return err(IteratorMethodsIteratedSourceError{
        .code = IteratorMethodsIteratedSourceErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(element);
}

IteratorMethods::ResultIteratorFromCall IteratorMethods::checkIteratorFrom(CallExpr *call,
                                                                           MemberExpr *member) {
  if (this->callChecker->rejectSpread(call)) {
    return err(IteratorMethodsCheckIteratorFromError{
        .code = IteratorMethodsCheckIteratorFromErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto found = GlobalMemberTable::find(GlobalName::ITERATOR, member->name);
  if (found != GlobalMember::ITERATOR_FROM) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextStaticFrom::iterator(member->name);

    return err(IteratorMethodsCheckIteratorFromError{
        .code = IteratorMethodsCheckIteratorFromErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(IteratorMethodsCheckIteratorFromError{
        .code = IteratorMethodsCheckIteratorFromErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  call->args[0] = this->dictChecker->iterableOf(this->exprChecker->checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  auto source = this->iteratedSource(type, "Iterator.from");
  if (source.isErr()) {
    bool isReported =
        source.error().code == IteratorMethodsIteratedSourceErrorCode::ALREADY_REPORTED;

    return err(IteratorMethodsCheckIteratorFromError{
        .code = isReported ? IteratorMethodsCheckIteratorFromErrorCode::ALREADY_REPORTED
                           : IteratorMethodsCheckIteratorFromErrorCode::NOT_ITERABLE,
        .message = source.error().message,
    });
  }
  call->target = BuiltinCall{.builtin = Builtin::IterFrom};
  call->type = type->isIterator() ? type : Type::iterator(source.value());

  return ok(call);
}

IteratorMethods::ResultArrayStaticKnown
IteratorMethods::arrayStatic(const MemberExpr *member) const {
  auto found = GlobalMemberTable::find(GlobalName::ARRAY, member->name);
  if (found != GlobalMember::ARRAY_FROM) {
    auto message = ErrorTextStaticFrom::array(member->name);

    return err(IteratorMethodsArrayStaticError{
        .code = IteratorMethodsArrayStaticErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(member);
}

IteratorMethods::ResultCallbackHasValue
IteratorMethods::callbackValue(const Type *result, llvm::StringRef functionName) const {
  if (result->isVoid() || result->isUndefined()) {
    auto message = ErrorTextCallbackValue::missing(functionName);

    return err(IteratorMethodsCallbackValueError{
        .code = IteratorMethodsCallbackValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(result);
}

Expr *IteratorMethods::checkArrayFrom(CallExpr *call, MemberExpr *member) {
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto known = this->arrayStatic(member);
  if (known.isErr()) {
    this->state.getDiagnostics().error(member->loc(), known.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  this->markFused(call->args[0]);
  call->args[0] = this->dictChecker->iterableOf(this->exprChecker->checkExpr(call->args[0]));
  const Type *type = call->args[0]->type;
  auto source = this->iteratedSource(type, "Array.from");
  if (source.isErr()) {
    if (source.error().code != IteratorMethodsIteratedSourceErrorCode::ALREADY_REPORTED) {
      this->state.getDiagnostics().error(call->args[0]->loc(), source.error().message);
    }
    if (call->args.size() == 2) {
      call->args[1] = this->exprChecker->checkExpr(call->args[1]);
    }
    return call;
  }
  const Type *element = source.value();
  if (type->isIterator()) {
    auto nothing = this->generatorChecker->checkSendsNothing(type, "'Array.from'");
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), nothing.error().message);
    }
  }
  call->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  call->type = Type::array(element);
  if (call->args.size() == 2) {
    Signature signature;
    signature.pad = false;
    signature.params = {element, Type::intTy()};
    call->args[1] =
        this->callbackChecker->checkFunctionArg(call->args[1], signature, 1, "Array.from");
    const Type *callback = call->args[1]->type;
    if (!callback->isFunction()) {
      return call;
    }
    auto value = this->callbackValue(callback->ret(), "Array.from");
    if (value.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
      call->type = Type::error();
      return call;
    }
    call->type = Type::array(value.value());
  }
  this->flowChecker->calledFunction();

  return call;
}

IteratorMethods::ResultNextValueOneType IteratorMethods::nextValue(const Type *iterator) const {
  const Type *element = iterator->yielded();
  const Type *returned = iterator->returned();
  const Type *value = returned->isVoid() ? element : Type::unionOf({element, returned});
  auto problem = UnionSlots::unionProblem(value);
  if (!problem.empty()) {
    auto message =
        ErrorTextNextValue::noUnion(iterator->name(), element->name(), returned->name(), problem);

    return err(IteratorMethodsNextValueError{
        .code = IteratorMethodsNextValueErrorCode::UNION_PROBLEM,
        .message = message,
    });
  }

  return ok(value);
}

IteratorMethods::ResultNotThrowOrReturn IteratorMethods::plainMethod(llvm::StringRef name) const {
  const auto *method = IteratorMethodTable::find(name);
  if (method != nullptr && method->method == IteratorMethod::THROW) {
    auto message = ErrorTextIteratorMethod::noThrow();

    return err(IteratorMethodsPlainMethodError{
        .code = IteratorMethodsPlainMethodErrorCode::NO_THROW,
        .message = message,
    });
  }
  if (method != nullptr && method->method == IteratorMethod::RETURN) {
    auto message = ErrorTextIteratorMethod::noReturn();

    return err(IteratorMethodsPlainMethodError{
        .code = IteratorMethodsPlainMethodErrorCode::NO_RETURN,
        .message = message,
    });
  }

  return ok(name);
}

IteratorMethods::ResultKnownHelperFound
IteratorMethods::iteratorHelper(llvm::StringRef name, const Type *iterator) const {
  const auto *helper = IteratorMethodTable::find(name);
  const IteratorStageMethod *stage = nullptr;
  const IteratorConsumerMethod *consumer = nullptr;
  if (helper != nullptr) {
    stage = helper->form.as<IteratorStageMethod>();
    consumer = helper->form.as<IteratorConsumerMethod>();
  }
  if (stage == nullptr && consumer == nullptr) {
    auto message = ErrorTextIteratorMethod::noProperty(name, iterator->name());

    return err(IteratorMethodsIteratorHelperError{
        .code = IteratorMethodsIteratorHelperErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }
  Builtin builtin = stage != nullptr ? stage->builtin : consumer->builtin;

  return ok(builtin);
}

IteratorMethods::ResultCallbackIsPassed
IteratorMethods::callbackGiven(size_t count, llvm::StringRef name, bool reduce) const {
  if (count == 0) {
    auto message = ErrorTextIteratorMethod::noFunction(name);

    return err(IteratorMethodsCallbackGivenError{
        .code = IteratorMethodsCallbackGivenErrorCode::NO_FUNCTION,
        .message = message,
    });
  }
  unsigned most = reduce ? 2u : 1u;
  if (count > most) {
    auto message = ErrorTextArity::count(1, most, count);

    return err(IteratorMethodsCallbackGivenError{
        .code = IteratorMethodsCallbackGivenErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(count);
}

IteratorMethods::ResultInitialValueType
IteratorMethods::initialValue(const Type *accumulator) const {
  if (accumulator->isUndefined() || accumulator->isVoid()) {
    auto message = ErrorTextReduceInit::untyped();

    return err(IteratorMethodsInitialValueError{
        .code = IteratorMethodsInitialValueErrorCode::UNTYPED_INITIAL,
        .message = message,
    });
  }

  return ok(accumulator);
}

IteratorMethods::ResultFlatMapInnerType IteratorMethods::flatMapInner(const Type *result) const {
  const Type *inner = result->isString() ? nullptr : this->generatorChecker->iteratedType(result);
  if (inner == nullptr) {
    auto message = ErrorTextFlatMap::notIter(result->name());

    return err(IteratorMethodsFlatMapInnerError{
        .code = IteratorMethodsFlatMapInnerErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(inner);
}

Expr *IteratorMethods::checkIteratorMethod(CallExpr *call, MemberExpr *member) {
  const auto *method = IteratorMethodTable::find(member->name);
  const IteratorStepMethod *step =
      method != nullptr ? method->form.as<IteratorStepMethod>() : nullptr;
  if (step != nullptr) {
    return this->checkIteratorStep(call, member, step->builtin);
  }
  auto helper = this->helperBuiltin(call, member);
  if (!helper.has_value()) {
    return call;
  }
  switch (method->method) {
    case IteratorMethod::TAKE:
    case IteratorMethod::DROP:
      return this->checkTakeDrop(call, member, *helper);
    case IteratorMethod::TO_ARRAY:
      return this->checkToArray(call, member, *helper);
    case IteratorMethod::MAP:
    case IteratorMethod::FILTER:
    case IteratorMethod::FLAT_MAP:
    case IteratorMethod::FOR_EACH:
    case IteratorMethod::REDUCE:
    case IteratorMethod::SOME:
    case IteratorMethod::EVERY:
    case IteratorMethod::FIND:
      return this->checkIteratorCallback(call, member, method->method, *helper);
    case IteratorMethod::NEXT:
    case IteratorMethod::RETURN:
    case IteratorMethod::THROW:
      return call;
  }

  return call;
}

bool IteratorMethods::hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min,
                                    unsigned max) {
  auto arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }

  return arity.isErr();
}

Expr *IteratorMethods::checkIteratorStep(CallExpr *call, MemberExpr *member, Builtin builtin) {
  const Type *iterator = member->object->type;
  if (this->hasWrongArity(call, member, 0, 1)) {
    return call;
  }
  auto next = this->nextValue(iterator);
  if (next.isErr()) {
    this->state.getDiagnostics().error(member->loc(), next.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  const Type *value = next.value();
  if (call->args.size() == 1) {
    const Type *sent = iterator->sent();
    call->args[0] = sent->isVoid() ? this->exprChecker->checkExpr(call->args[0])
                                   : this->argumentChecker->checkArg(call->args[0], sent);
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type =
      Type::object({{"value", Type::optional(value), false}, {"done", Type::boolTy(), false}},
                   "IteratorResult<" + value->name().str() + ">");
  this->flowChecker->calledFunction();

  return call;
}

std::optional<Builtin> IteratorMethods::helperBuiltin(CallExpr *call, MemberExpr *member) {
  const Type *iterator = member->object->type;
  llvm::StringRef name = member->name;
  auto plain = this->plainMethod(name);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(member->loc(), plain.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return std::nullopt;
  }
  auto found = this->iteratorHelper(name, iterator);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return std::nullopt;
  }
  auto what = ErrorTextFunctionName::quoted(name);
  auto nothing = this->generatorChecker->checkSendsNothing(iterator, what);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(member->loc(), nothing.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return std::nullopt;
  }

  return found.value();
}

Expr *IteratorMethods::checkTakeDrop(CallExpr *call, MemberExpr *member, Builtin builtin) {
  if (this->hasWrongArity(call, member, 1, 1)) {
    return call;
  }
  call->args[0] = this->memberChecker->checkPosition(call->args[0]);
  auto counted = ErrorTextIteratorMethod::countOf(member->name);
  auto between = this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, counted);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::iterator(member->object->type->yielded());

  return call;
}

Expr *IteratorMethods::checkToArray(CallExpr *call, MemberExpr *member, Builtin builtin) {
  if (this->hasWrongArity(call, member, 0, 0)) {
    return call;
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::array(member->object->type->yielded());
  this->flowChecker->calledFunction();

  return call;
}

Expr *IteratorMethods::checkIteratorCallback(CallExpr *call, MemberExpr *member,
                                             IteratorMethod method, Builtin builtin) {
  const Type *element = member->object->type->yielded();
  llvm::StringRef name = member->name;
  size_t count = call->args.size();
  bool isReduce = method == IteratorMethod::REDUCE;
  auto passed = this->callbackGiven(count, name, isReduce);
  if (passed.isErr()) {
    this->state.getDiagnostics().error(member->loc(), passed.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  const Type *accumulator = isReduce && count == 2 ? this->reduceInitType(call, element) : element;
  if (accumulator->isError()) {
    call->args[0] = this->exprChecker->checkExpr(call->args[0]);
    return call;
  }
  auto signature = this->callbackSignature(method, element, accumulator);
  bool isBoolean = this->callChecker->isGlobalName(call->args[0], GlobalName::BOOLEAN);
  auto *given = dyn_cast<FunctionExpr>(call->args[0]);
  if (given != nullptr) {
    this->state.getCallbackFacts().rememberIndexCallback(given->fn, isReduce ? 2 : 1);
  }
  unsigned primary = isReduce ? 2 : 1;
  call->args[0] = this->callbackChecker->checkFunctionArg(call->args[0], signature, primary, name);
  auto *checked = dyn_cast<FunctionExpr>(call->args[0]);
  if (BuiltinGroups::isIteratorStage(builtin) && checked != nullptr) {
    checked->fn->nonEscaping = call->fused;
  }
  call->target = BuiltinCall{.builtin = builtin};
  if (BuiltinGroups::isIteratorSink(builtin)) {
    this->flowChecker->calledFunction();
  }
  if (!call->args[0]->type->isFunction()) {
    return call;
  }
  IteratorCallback callback{
      .call = call,
      .name = name,
      .element = element,
      .accumulator = accumulator,
      .isBoolean = isBoolean,
      .count = count,
  };

  return this->checkCallbackResult(callback, method);
}

const Type *IteratorMethods::reduceInitType(CallExpr *call, const Type *element) {
  Expr *init = call->args[1];
  auto *unary = dyn_cast<UnaryExpr>(init);
  bool isNegative =
      unary != nullptr && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand);
  bool isLiteral = isa<IntLitExpr>(init) || isNegative;
  const auto *apparent = this->genericChecker->apparentType(element);
  const Type *hint = isLiteral && apparent->isNumeric() ? apparent : nullptr;
  call->args[1] = this->exprChecker->checkExpr(init, hint);
  const Type *accumulator = call->args[1]->type;
  auto initial = this->initialValue(accumulator);
  if (initial.isErr()) {
    this->state.getDiagnostics().error(call->args[1]->loc(), initial.error().message);
    return Type::error();
  }

  return accumulator;
}

Signature IteratorMethods::callbackSignature(IteratorMethod method, const Type *element,
                                             const Type *accumulator) const {
  Signature signature;
  signature.pad = false;
  if (method == IteratorMethod::FOR_EACH) {
    signature.voidResult = true;
  }
  bool isPredicate = method == IteratorMethod::FILTER || method == IteratorMethod::SOME ||
                     method == IteratorMethod::EVERY || method == IteratorMethod::FIND;
  if (isPredicate) {
    signature.ret = Type::boolTy();
  }
  bool isReduce = method == IteratorMethod::REDUCE;
  if (isReduce) {
    signature.ret = accumulator;
  }
  signature.params = isReduce ? std::vector<const Type *>{accumulator, element, Type::intTy()}
                              : std::vector<const Type *>{element, Type::intTy()};

  return signature;
}

Expr *IteratorMethods::checkCallbackResult(const IteratorCallback &callback,
                                           IteratorMethod method) {
  CallExpr *call = callback.call;
  const Type *element = callback.element;
  const Type *result = call->args[0]->type->ret();
  switch (method) {
    case IteratorMethod::MAP:
      return this->checkMapResult(callback, result);
    case IteratorMethod::FLAT_MAP:
      return this->checkFlatMapResult(call, result);
    case IteratorMethod::FILTER:
      call->type = Type::iterator(this->filteredElement(callback));
      return call;
    case IteratorMethod::FOR_EACH:
      call->type = Type::voidTy();
      return call;
    case IteratorMethod::REDUCE:
      return this->finishReduce(callback);
    case IteratorMethod::SOME:
    case IteratorMethod::EVERY:
      call->type = Type::boolTy();
      return call;
    case IteratorMethod::FIND:
      call->type = Type::optional(element);
      return call;
    case IteratorMethod::NEXT:
    case IteratorMethod::RETURN:
    case IteratorMethod::THROW:
    case IteratorMethod::TAKE:
    case IteratorMethod::DROP:
    case IteratorMethod::TO_ARRAY:
      return call;
  }

  return call;
}

Expr *IteratorMethods::checkMapResult(const IteratorCallback &callback, const Type *result) {
  CallExpr *call = callback.call;
  auto value = this->callbackValue(result, callback.name);
  if (value.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), value.error().message);
    return call;
  }
  call->type = Type::iterator(result);

  return call;
}

Expr *IteratorMethods::checkFlatMapResult(CallExpr *call, const Type *result) {
  auto inner = this->flatMapInner(result);
  if (inner.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), inner.error().message);
    return call;
  }
  if (result->isIterator()) {
    auto flatSends = this->generatorChecker->checkSendsNothing(result, "'flatMap'");
    if (flatSends.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), flatSends.error().message);
      return call;
    }
  }
  call->type = Type::iterator(inner.value());

  return call;
}

const Type *IteratorMethods::filteredElement(const IteratorCallback &callback) {
  const Expr *predicate = callback.call->args[0];
  const Type *element = callback.element;
  const auto *kept = this->predicateChecker->filteredType(predicate, element);
  if (kept != nullptr) {
    return kept;
  }
  bool isDefined = element->isOptional() &&
                   (callback.isBoolean || this->callbackChecker->keepsDefined(predicate));

  return isDefined ? element->inner() : element;
}

Expr *IteratorMethods::finishReduce(const IteratorCallback &callback) {
  CallExpr *call = callback.call;
  const Type *accumulator = callback.accumulator;
  call->type = callback.count == 2 ? accumulator : Type::optional(accumulator);
  if (callback.count == 2 && call->args.size() == 2) {
    call->args[1] = this->typeRelations->coerce(call->args[1], accumulator);
  }

  return call;
}

}
