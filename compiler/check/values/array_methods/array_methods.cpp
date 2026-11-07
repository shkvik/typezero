#include "compiler/check/values/array_methods/array_methods.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
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

using llvm::dyn_cast;

namespace tz {

const VarSymbol *ArrayMethods::arrayVariable(const Expr *object) {
  if (auto *conv = dyn_cast<CastExpr>(object);
      conv && conv->isImplicit() && conv->operand->type->isOptional()) {
    object = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(object);
  if (!name || !name->var() || !name->var()->type->nonOptional()->isArray()) {
    return nullptr;
  }

  return name->var();
}

ArrayMethods::ArrayMethods(Checker &owner)
    : checker(owner) {}

ArrayMethods::ResultElementsAreText ArrayMethods::textElements(const Type *element,
                                                               llvm::StringRef name) {
  if (Checker::holdsFunctions(element)) {
    std::string message = ErrorTextArrayText::functions();

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::FUNCTIONS,
        .message = message,
    });
  }
  if (Checker::holdsResults(element)) {
    std::string message = ErrorTextArrayText::results(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::RESULTS,
        .message = message,
    });
  }
  if (UnknownChecker::holdsUnknown(element)) {
    std::string message = ErrorTextArrayText::unknown(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::UNKNOWN,
        .message = message,
    });
  }
  if (JsonChecker::holdsJson(element)) {
    std::string message = ErrorTextArrayText::json(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::JSON,
        .message = message,
    });
  }
  if (Checker::holdsObjects(element) && !Checker::holdsDates(element)) {
    std::string message = ErrorTextArrayText::objects(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::OBJECTS,
        .message = message,
    });
  }

  return ok(element);
}

ArrayMethods::ResultSortableElement ArrayMethods::sortable(const Type *element) {
  if (Checker::holdsFunctions(element)) {
    std::string message = ErrorTextArraySort::functions();

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::FUNCTIONS,
        .message = message,
    });
  }
  if (UnknownChecker::holdsUnknown(element)) {
    std::string message = ErrorTextArraySort::unknown();

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::UNKNOWN,
        .message = message,
    });
  }
  if (JsonChecker::holdsJson(element)) {
    std::string message = ErrorTextArraySort::json();

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::JSON,
        .message = message,
    });
  }
  if (Checker::holdsDates(element)) {
    std::string message = ErrorTextArraySort::dates();

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::DATES,
        .message = message,
    });
  }
  if (Checker::holdsObjects(element) || Checker::holdsResults(element)) {
    std::string message = ErrorTextArraySort::objects();

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::OBJECTS,
        .message = message,
    });
  }
  auto textual = [](const Type *m) {
    const Type *b = m->isLiteral() ? m->literalBase() : m;

    return b->isString() || b->isBool();
  };
  const Type *u = element->nonOptional();
  if (u->isUnion() && !llvm::all_of(u->members(), textual)) {
    std::string message = ErrorTextArraySort::mixed(u->name());

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::MIXED_UNION,
        .message = message,
    });
  }
  bool numbers = u->isNumeric() || u->isDecimal() || GenericChecker::apparentType(u)->isNumeric();
  if (numbers) {
    std::string message = ErrorTextArraySort::numbers(element->isOptional());

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::NUMBERS,
        .message = message,
    });
  }

  return ok(element);
}

ArrayMethods::ResultArrayMethodCall
ArrayMethods::checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  const VarSymbol *self = ArrayMethods::arrayVariable(member->object);
  llvm::StringRef name = member->name;
  size_t count = call->args.size();

  auto arity = [&](unsigned min, unsigned max) -> ResultArrayMethodCall {
    if (count >= min && count <= max) {
      return ok(call);
    }
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextArity::count(min, max, count);

    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::WRONG_ARITY,
        .message = message,
    });
  };
  auto elementArg = [&](size_t i) {
    call->args[i] = this->checker.typeRelations().coerce(
        this->checker.exprChecker().checkExpr(call->args[i], element), element);
  };
  auto positionArgs = [&](size_t from) {
    for (size_t i = from; i < count; ++i) {
      call->args[i] = this->checker.memberChecker().checkPosition(call->args[i]);
    }
  };

  static const char *const callbacks[] = {"map",         "filter",  "forEach",   "reduce",
                                          "reduceRight", "find",    "findIndex", "some",
                                          "every",       "flatMap", "findLast",  "findLastIndex"};
  if (llvm::is_contained(callbacks, name) || (name == "sort" && count != 0)) {
    Expr *checked = this->checker.callbackChecker().checkCallbackMethod(call, member, expected);

    return ok(checked);
  }
  if (name == "keys" || name == "values" || name == "entries") {
    IteratorMethods::ResultArrayIterateCall iterated =
        this->checker.iteratorMethods().checkArrayIterator(call, member);
    if (iterated.isErr()) {
      return err(ArrayMethodsCheckArrayMethodError{
          .code = ArrayMethodsCheckArrayMethodErrorCode::WRONG_ARITY,
          .message = iterated.error().message,
      });
    }
    return ok(iterated.value());
  }

  if (name == "push" || name == "unshift") {
    bool spreads = call->hasSpread();
    for (size_t i = 0; i < count; ++i) {
      if (call->spreads(i)) {
        call->args[i] = this->checker.typeRelations().coerce(
            this->checker.exprChecker().checkExpr(call->args[i], arrayType), arrayType);
      } else {
        elementArg(i);
      }
    }
    call->target = BuiltinCall{.builtin = name == "push" ? Builtin::ArrPush : Builtin::ArrUnshift};
    call->type = Type::intTy();
    if (self && !spreads) {
      this->checker.flow_.facts.shift(Term::lengthOf(self), static_cast<int64_t>(count));
    }
    this->checker.flowChecker().arrayResized(arrayType, true, false, spreads ? nullptr : self);
    return ok(call);
  }

  if (name == "pop" || name == "shift") {
    ResultArrayMethodCall counted = arity(0, 0);
    if (counted.isErr()) {
      return counted;
    }
    call->target = BuiltinCall{.builtin = name == "pop" ? Builtin::ArrPop : Builtin::ArrShift};
    bool notEmpty = this->checker.indexProofs().provesNotEmpty(member->object);
    call->mayBeOutOfRange = !notEmpty;
    call->type = notEmpty ? element : Type::optional(element);
    if (self) {
      if (notEmpty) {
        this->checker.flow_.facts.shift(Term::lengthOf(self), -1);
      } else {
        this->checker.flow_.facts.forgetIf(
            [&](const Facts::Fact &f) { return f.rhs == Term::lengthOf(self); });
      }
    }
    this->checker.flowChecker().arrayResized(arrayType, false, true, self);
    return ok(call);
  }

  if (name == "at") {
    ResultArrayMethodCall counted = arity(1, 1);
    if (counted.isErr()) {
      return counted;
    }
    positionArgs(0);
    call->target = BuiltinCall{.builtin = Builtin::ArrAt};
    call->mayBeOutOfRange =
        !this->checker.indexProofs().provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(element) : element;
    return ok(call);
  }

  if (name == "slice") {
    ResultArrayMethodCall counted = arity(0, 2);
    if (counted.isErr()) {
      return counted;
    }
    positionArgs(0);
    call->target = BuiltinCall{.builtin = Builtin::ArrSlice};
    call->type = arrayType;
    return ok(call);
  }

  if (name == "concat") {
    for (size_t i = 0; i < count; ++i) {
      call->args[i] = this->checker.exprChecker().checkExpr(call->args[i], arrayType);
      if (call->args[i]->type != arrayType) {
        call->args[i] = this->checker.typeRelations().coerce(call->args[i], element);
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::ArrConcat};
    call->type = arrayType;
    return ok(call);
  }

  if (name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    ResultArrayMethodCall counted = arity(1, 2);
    if (counted.isErr()) {
      return counted;
    }
    if (Checker::holdsResults(element)) {
      this->checker.argumentChecker().checkArgsLoosely(call);
      std::string message = ErrorTextArrayMethod::results(name);

      return err(ArrayMethodsCheckArrayMethodError{
          .code = ArrayMethodsCheckArrayMethodErrorCode::RESULTS_COMPARED,
          .message = message,
      });
    }
    if (element->nonOptional()->isFunction()) {
      this->checker.file_->comparesFunctions = true;
    }
    elementArg(0);
    positionArgs(1);
    call->target = BuiltinCall{.builtin = name == "indexOf"       ? Builtin::ArrIndexOf
                                          : name == "lastIndexOf" ? Builtin::ArrLastIndexOf
                                                                  : Builtin::ArrIncludes};
    call->type = name == "includes" ? Type::boolTy() : Type::intTy();
    return ok(call);
  }

  if (name == "join" || name == "toString") {
    ResultElementsAreText text = ArrayMethods::textElements(element, name);
    if (text.isErr()) {
      this->checker.argumentChecker().checkArgsLoosely(call);
      return err(ArrayMethodsCheckArrayMethodError{
          .code = ArrayMethodsCheckArrayMethodErrorCode::ELEMENTS_NOT_TEXT,
          .message = text.error().message,
      });
    }
  }
  if (name == "join") {
    ResultArrayMethodCall counted = arity(0, 1);
    if (counted.isErr()) {
      return counted;
    }
    if (count == 1) {
      call->args[0] = this->checker.typeRelations().coerce(
          this->checker.exprChecker().checkExpr(call->args[0], Type::string()), Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::ArrJoin};
    call->type = Type::string();
    return ok(call);
  }

  if (name == "reverse" || name == "toString") {
    ResultArrayMethodCall counted = arity(0, 0);
    if (counted.isErr()) {
      return counted;
    }
    call->target =
        BuiltinCall{.builtin = name == "reverse" ? Builtin::ArrReverse : Builtin::ArrToString};
    call->type = name == "reverse" ? arrayType : Type::string();
    return ok(call);
  }

  if (name == "fill") {
    ResultArrayMethodCall counted = arity(1, 3);
    if (counted.isErr()) {
      return counted;
    }
    elementArg(0);
    positionArgs(1);
    call->target = BuiltinCall{.builtin = Builtin::ArrFill};
    call->type = arrayType;
    return ok(call);
  }

  if (name == "sort") {
    ResultSortableElement ordered = ArrayMethods::sortable(element);
    if (ordered.isErr()) {
      return err(ArrayMethodsCheckArrayMethodError{
          .code = ArrayMethodsCheckArrayMethodErrorCode::NEEDS_COMPARISON,
          .message = ordered.error().message,
      });
    }
    call->target = BuiltinCall{.builtin = Builtin::ArrSort};
    call->type = arrayType;
    return ok(call);
  }

  if (name == "splice") {
    ResultArrayMethodCall counted = arity(1, ~0u);
    if (counted.isErr()) {
      return counted;
    }
    for (size_t i = 0; i < count; ++i) {
      if (i < 2) {
        call->args[i] = this->checker.memberChecker().checkPosition(call->args[i]);
      } else {
        elementArg(i);
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::ArrSplice};
    call->type = arrayType;
    this->checker.flowChecker().arrayResized(arrayType, true, true);
    return ok(call);
  }
  this->checker.argumentChecker().checkArgsLoosely(call);
  std::string message = ErrorTextArrayMethod::noProperty(name, arrayType->name());

  return err(ArrayMethodsCheckArrayMethodError{
      .code = ArrayMethodsCheckArrayMethodErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

}
