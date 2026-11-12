#include "compiler/semantic/bodies/values/array_methods/array_methods.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>
#include <functional>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr unsigned kAnyCount = ~0u;
constexpr size_t kSplicePositions = 2;

}

ArrayMethods::ArrayMethods(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->jsonChecker = &all.jsonChecker;
  this->memberChecker = &all.memberChecker;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->unknownChecker = &all.unknownChecker;
  this->argumentChecker = &all.argumentChecker;
  this->callbackChecker = &all.callbackChecker;
  this->iteratorMethods = &all.iteratorMethods;
}

const VarSymbol *ArrayMethods::arrayVariable(const Expr *object) const {
  auto *conversion = dyn_cast<CastExpr>(object);
  bool isOptionalWrap =
      conversion != nullptr && conversion->isImplicit() && conversion->operand->type->isOptional();
  const Expr *inner = isOptionalWrap ? conversion->operand : object;
  auto *name = dyn_cast<NameExpr>(inner);
  if (name == nullptr || name->var() == nullptr || !name->var()->type->nonOptional()->isArray()) {
    return nullptr;
  }

  return name->var();
}

ArrayMethods::ResultElementsAreText ArrayMethods::checkTextElements(const Type *element,
                                                                    llvm::StringRef name) const {
  if (element->holdsFunctions()) {
    auto message = ErrorTextArrayText::functions();

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::FUNCTIONS,
        .message = message,
    });
  }
  if (element->holdsResults()) {
    auto message = ErrorTextArrayText::results(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::RESULTS,
        .message = message,
    });
  }
  if (this->unknownChecker->isHoldingUnknown(element)) {
    auto message = ErrorTextArrayText::unknown(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::UNKNOWN,
        .message = message,
    });
  }
  if (this->jsonChecker->holdsJson(element)) {
    auto message = ErrorTextArrayText::json(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::JSON,
        .message = message,
    });
  }
  if (element->holdsObjects() && !element->holdsDates()) {
    auto message = ErrorTextArrayText::objects(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::OBJECTS,
        .message = message,
    });
  }

  return ok(element);
}

ArrayMethods::ResultSortableElement ArrayMethods::checkSortable(const Type *element) const {
  auto problem = this->sortProblem(element);
  if (!problem.has_value()) {
    return ok(element);
  }
  auto message = this->sortMessage(*problem, element);

  return err(ArrayMethodsSortableError{
      .code = *problem,
      .message = message,
  });
}

std::optional<ArrayMethodsSortableErrorCode> ArrayMethods::sortProblem(const Type *element) const {
  if (element->holdsFunctions()) {
    return ArrayMethodsSortableErrorCode::FUNCTIONS;
  }
  if (this->unknownChecker->isHoldingUnknown(element)) {
    return ArrayMethodsSortableErrorCode::UNKNOWN;
  }
  if (this->jsonChecker->holdsJson(element)) {
    return ArrayMethodsSortableErrorCode::JSON;
  }
  if (element->holdsDates()) {
    return ArrayMethodsSortableErrorCode::DATES;
  }
  if (element->holdsObjects() || element->holdsResults()) {
    return ArrayMethodsSortableErrorCode::OBJECTS;
  }
  const Type *base = element->nonOptional();
  if (base->isUnion() && !this->areAllTextual(base->members())) {
    return ArrayMethodsSortableErrorCode::MIXED_UNION;
  }
  bool hasNumbers = base->isNumeric() || base->isDecimal() ||
                    this->genericChecker->apparentType(base)->isNumeric();
  if (hasNumbers) {
    return ArrayMethodsSortableErrorCode::NUMBERS;
  }

  return std::nullopt;
}

bool ArrayMethods::areAllTextual(llvm::ArrayRef<const Type *> members) const {
  for (const Type *member : members) {
    const Type *memberBase = member->isLiteral() ? member->literalBase() : member;
    if (!memberBase->isString() && !memberBase->isBool()) {
      return false;
    }
  }

  return true;
}

std::string ArrayMethods::sortMessage(ArrayMethodsSortableErrorCode code,
                                      const Type *element) const {
  switch (code) {
    case ArrayMethodsSortableErrorCode::FUNCTIONS:
      return ErrorTextArraySort::functions();
    case ArrayMethodsSortableErrorCode::UNKNOWN:
      return ErrorTextArraySort::unknown();
    case ArrayMethodsSortableErrorCode::JSON:
      return ErrorTextArraySort::json();
    case ArrayMethodsSortableErrorCode::DATES:
      return ErrorTextArraySort::dates();
    case ArrayMethodsSortableErrorCode::OBJECTS:
      return ErrorTextArraySort::objects();
    case ArrayMethodsSortableErrorCode::MIXED_UNION:
      return ErrorTextArraySort::mixed(element->nonOptional()->name());
    case ArrayMethodsSortableErrorCode::NUMBERS:
      return ErrorTextArraySort::numbers(element->isOptional());
  }

  return ErrorTextArraySort::objects();
}

ArrayMethods::ResultArrayMethodCall
ArrayMethods::checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *arrayType = member->object->type;
  const auto *method = ArrayMethodTable::find(member->name);
  if (method == nullptr) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextArrayMethod::noProperty(member->name, arrayType->name());

    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }
  auto arrayCall = ArrayCall{
      .call = call,
      .member = member,
      .expected = expected,
      .arrayType = arrayType,
      .element = arrayType->inner(),
      .self = this->arrayVariable(member->object),
      .count = call->args.size(),
      .method = method,
  };
  switch (method->method) {
    case ArrayMethod::FOR_EACH:
    case ArrayMethod::MAP:
    case ArrayMethod::FLAT_MAP:
    case ArrayMethod::FILTER:
    case ArrayMethod::REDUCE:
    case ArrayMethod::REDUCE_RIGHT:
    case ArrayMethod::FIND:
    case ArrayMethod::FIND_LAST:
    case ArrayMethod::FIND_INDEX:
    case ArrayMethod::FIND_LAST_INDEX:
    case ArrayMethod::SOME:
    case ArrayMethod::EVERY:
      return this->checkCallbackCall(arrayCall);
    case ArrayMethod::KEYS:
    case ArrayMethod::VALUES:
    case ArrayMethod::ENTRIES:
      return this->checkIteratorCall(arrayCall);
    case ArrayMethod::PUSH:
    case ArrayMethod::UNSHIFT:
      return this->checkGrowingCall(arrayCall);
    case ArrayMethod::POP:
    case ArrayMethod::SHIFT:
      return this->checkShrinkingCall(arrayCall);
    case ArrayMethod::AT:
      return this->checkAtCall(arrayCall);
    case ArrayMethod::SLICE:
      return this->checkSliceCall(arrayCall);
    case ArrayMethod::CONCAT:
      return this->checkConcatCall(arrayCall);
    case ArrayMethod::INDEX_OF:
    case ArrayMethod::LAST_INDEX_OF:
    case ArrayMethod::INCLUDES:
      return this->checkSearchCall(arrayCall);
    case ArrayMethod::JOIN:
      return this->checkJoinCall(arrayCall);
    case ArrayMethod::TO_STRING:
      return this->checkToStringCall(arrayCall);
    case ArrayMethod::REVERSE:
      return this->checkReverseCall(arrayCall);
    case ArrayMethod::FILL:
      return this->checkFillCall(arrayCall);
    case ArrayMethod::SORT:
      return this->checkSortCall(arrayCall);
    case ArrayMethod::SPLICE:
      return this->checkSpliceCall(arrayCall);
  }

  return ok(call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkArity(const ArrayCall &arrayCall,
                                                             unsigned min, unsigned max) {
  size_t count = arrayCall.count;
  if (count >= min && count <= max) {
    return ok(arrayCall.call);
  }
  this->argumentChecker->walkArgsLoosely(arrayCall.call);
  auto message = ErrorTextArity::count(min, max, count);

  return err(ArrayMethodsCheckArrayMethodError{
      .code = ArrayMethodsCheckArrayMethodErrorCode::WRONG_ARITY,
      .message = message,
  });
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkTextArity(const ArrayCall &arrayCall,
                                                                 unsigned max) {
  auto text = this->checkTextElements(arrayCall.element, arrayCall.member->name);
  if (text.isErr()) {
    this->argumentChecker->walkArgsLoosely(arrayCall.call);
    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::ELEMENTS_NOT_TEXT,
        .message = text.error().message,
    });
  }

  return this->checkArity(arrayCall, 0, max);
}

void ArrayMethods::walkElementArg(const ArrayCall &arrayCall, size_t index) {
  Expr *&arg = arrayCall.call->args[index];
  auto *checked = this->exprChecker->checkExpr(arg, arrayCall.element);
  arg = this->typeRelations->coerce(checked, arrayCall.element);
}

void ArrayMethods::walkPositionArgs(const ArrayCall &arrayCall, size_t from) {
  for (size_t index = from; index < arrayCall.count; ++index) {
    Expr *&arg = arrayCall.call->args[index];
    arg = this->memberChecker->checkPosition(arg);
  }
}

void ArrayMethods::finishCall(const ArrayCall &arrayCall, const Type *type) {
  arrayCall.call->target = BuiltinCall{.builtin = arrayCall.method->builtin};
  arrayCall.call->type = type;
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkCallbackCall(const ArrayCall &arrayCall) {
  auto *checked = this->callbackChecker->checkCallbackMethod(arrayCall.call, arrayCall.member,
                                                             arrayCall.expected);

  return ok(checked);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkIteratorCall(const ArrayCall &arrayCall) {
  auto iterated = this->iteratorMethods->checkArrayIterator(arrayCall.call, arrayCall.member);
  if (iterated.isErr()) {
    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::WRONG_ARITY,
        .message = iterated.error().message,
    });
  }

  return ok(iterated.value());
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkGrowingCall(const ArrayCall &arrayCall) {
  CallExpr *call = arrayCall.call;
  bool isSpreading = call->hasSpread();
  for (size_t index = 0; index < arrayCall.count; ++index) {
    this->walkPushedArg(arrayCall, index);
  }
  this->finishCall(arrayCall, Type::intTy());
  if (arrayCall.self != nullptr && !isSpreading) {
    auto added = static_cast<int64_t>(arrayCall.count);
    this->state.getFlowState().getFacts().shift(Term::lengthOf(arrayCall.self), added);
  }
  const VarSymbol *except = isSpreading ? nullptr : arrayCall.self;
  this->flowChecker->arrayResized(arrayCall.arrayType, Resize::GROWS, except);

  return ok(call);
}

void ArrayMethods::walkPushedArg(const ArrayCall &arrayCall, size_t index) {
  if (!arrayCall.call->spreads(index)) {
    this->walkElementArg(arrayCall, index);
    return;
  }
  Expr *&arg = arrayCall.call->args[index];
  auto *checked = this->exprChecker->checkExpr(arg, arrayCall.arrayType);
  arg = this->typeRelations->coerce(checked, arrayCall.arrayType);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkShrinkingCall(const ArrayCall &arrayCall) {
  auto counted = this->checkArity(arrayCall, 0, 0);
  if (counted.isErr()) {
    return counted;
  }
  bool isNotEmpty = this->indexProofs->canProveNotEmpty(arrayCall.member->object);
  const Type *element = arrayCall.element;
  this->finishCall(arrayCall, isNotEmpty ? element : Type::optional(element));
  arrayCall.call->mayBeOutOfRange = !isNotEmpty;
  if (arrayCall.self != nullptr) {
    this->shrinkLength(arrayCall.self, isNotEmpty);
  }
  this->flowChecker->arrayResized(arrayCall.arrayType, Resize::SHRINKS, arrayCall.self);

  return ok(arrayCall.call);
}

void ArrayMethods::shrinkLength(const VarSymbol *self, bool isNotEmpty) {
  auto &facts = this->state.getFlowState().getFacts();
  if (isNotEmpty) {
    facts.shift(Term::lengthOf(self), -1);
    return;
  }
  auto isLengthFact = std::bind_front(&ArrayMethods::isLengthFact, this, self);
  facts.forgetIf(isLengthFact);
}

bool ArrayMethods::isLengthFact(const VarSymbol *self, const Facts::Fact &fact) const {
  return fact.rhs == Term::lengthOf(self);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkAtCall(const ArrayCall &arrayCall) {
  auto counted = this->checkArity(arrayCall, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  CallExpr *call = arrayCall.call;
  this->walkPositionArgs(arrayCall, 0);
  bool isInBounds = this->indexProofs->canProveAtInBounds(arrayCall.member->object, call->args[0]);
  const Type *element = arrayCall.element;
  this->finishCall(arrayCall, isInBounds ? element : Type::optional(element));
  call->mayBeOutOfRange = !isInBounds;

  return ok(call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkSliceCall(const ArrayCall &arrayCall) {
  auto counted = this->checkArity(arrayCall, 0, 2);
  if (counted.isErr()) {
    return counted;
  }
  this->walkPositionArgs(arrayCall, 0);
  this->finishCall(arrayCall, arrayCall.arrayType);

  return ok(arrayCall.call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkConcatCall(const ArrayCall &arrayCall) {
  for (size_t index = 0; index < arrayCall.count; ++index) {
    this->walkConcatArg(arrayCall, index);
  }
  this->finishCall(arrayCall, arrayCall.arrayType);

  return ok(arrayCall.call);
}

void ArrayMethods::walkConcatArg(const ArrayCall &arrayCall, size_t index) {
  Expr *&arg = arrayCall.call->args[index];
  arg = this->exprChecker->checkExpr(arg, arrayCall.arrayType);
  if (arg->type != arrayCall.arrayType) {
    arg = this->typeRelations->coerce(arg, arrayCall.element);
  }
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkSearchCall(const ArrayCall &arrayCall) {
  auto counted = this->checkArity(arrayCall, 1, 2);
  if (counted.isErr()) {
    return counted;
  }
  const Type *element = arrayCall.element;
  if (element->holdsResults()) {
    this->argumentChecker->walkArgsLoosely(arrayCall.call);
    auto message = ErrorTextArrayMethod::results(arrayCall.member->name);

    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }
  if (element->nonOptional()->isFunction()) {
    this->state.getInventoryStore().markComparesFunctions();
  }
  this->walkElementArg(arrayCall, 0);
  this->walkPositionArgs(arrayCall, 1);
  bool isIncludes = arrayCall.method->method == ArrayMethod::INCLUDES;
  this->finishCall(arrayCall, isIncludes ? Type::boolTy() : Type::intTy());

  return ok(arrayCall.call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkJoinCall(const ArrayCall &arrayCall) {
  auto counted = this->checkTextArity(arrayCall, 1);
  if (counted.isErr()) {
    return counted;
  }
  if (arrayCall.count == 1) {
    Expr *&separator = arrayCall.call->args[0];
    auto *checked = this->exprChecker->checkExpr(separator, Type::string());
    separator = this->typeRelations->coerce(checked, Type::string());
  }
  this->finishCall(arrayCall, Type::string());

  return ok(arrayCall.call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkToStringCall(const ArrayCall &arrayCall) {
  auto counted = this->checkTextArity(arrayCall, 0);
  if (counted.isErr()) {
    return counted;
  }
  this->finishCall(arrayCall, Type::string());

  return ok(arrayCall.call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkReverseCall(const ArrayCall &arrayCall) {
  auto counted = this->checkArity(arrayCall, 0, 0);
  if (counted.isErr()) {
    return counted;
  }
  this->finishCall(arrayCall, arrayCall.arrayType);

  return ok(arrayCall.call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkFillCall(const ArrayCall &arrayCall) {
  auto counted = this->checkArity(arrayCall, 1, 3);
  if (counted.isErr()) {
    return counted;
  }
  this->walkElementArg(arrayCall, 0);
  this->walkPositionArgs(arrayCall, 1);
  this->finishCall(arrayCall, arrayCall.arrayType);

  return ok(arrayCall.call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkSortCall(const ArrayCall &arrayCall) {
  if (arrayCall.count != 0) {
    return this->checkCallbackCall(arrayCall);
  }
  auto ordered = this->checkSortable(arrayCall.element);
  if (ordered.isErr()) {
    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::NEEDS_COMPARISON,
        .message = ordered.error().message,
    });
  }
  this->finishCall(arrayCall, arrayCall.arrayType);

  return ok(arrayCall.call);
}

ArrayMethods::ResultArrayMethodCall ArrayMethods::checkSpliceCall(const ArrayCall &arrayCall) {
  auto counted = this->checkArity(arrayCall, 1, kAnyCount);
  if (counted.isErr()) {
    return counted;
  }
  for (size_t index = 0; index < arrayCall.count; ++index) {
    this->walkSplicedArg(arrayCall, index);
  }
  this->finishCall(arrayCall, arrayCall.arrayType);
  this->flowChecker->arrayResized(arrayCall.arrayType, Resize::CHANGES);

  return ok(arrayCall.call);
}

void ArrayMethods::walkSplicedArg(const ArrayCall &arrayCall, size_t index) {
  if (index >= kSplicePositions) {
    this->walkElementArg(arrayCall, index);
    return;
  }
  Expr *&arg = arrayCall.call->args[index];
  arg = this->memberChecker->checkPosition(arg);
}

}
