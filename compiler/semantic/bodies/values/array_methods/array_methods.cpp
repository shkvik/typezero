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
#include "llvm/Support/ErrorHandling.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

ArrayMethods::ArrayMethods(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->indexProofs = &all.getIndexProofs();
  this->memberChecker = &all.getMemberChecker();
  this->typeRelations = &all.getTypeRelations();
  this->argumentChecker = &all.getArgumentChecker();
  this->callbackChecker = &all.getCallbackChecker();
  this->iteratorMethods = &all.getIteratorMethods();
}

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

ArrayMethods::ResultElementsAreText ArrayMethods::textElements(const Type *element,
                                                               llvm::StringRef name) {
  if (element->holdsFunctions()) {
    std::string message = ErrorTextArrayText::functions();

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::FUNCTIONS,
        .message = message,
    });
  }
  if (element->holdsResults()) {
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
  if (element->holdsObjects() && !element->holdsDates()) {
    std::string message = ErrorTextArrayText::objects(name);

    return err(ArrayMethodsTextElementsError{
        .code = ArrayMethodsTextElementsErrorCode::OBJECTS,
        .message = message,
    });
  }

  return ok(element);
}

ArrayMethods::ResultSortableElement ArrayMethods::sortable(const Type *element) {
  if (element->holdsFunctions()) {
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
  if (element->holdsDates()) {
    std::string message = ErrorTextArraySort::dates();

    return err(ArrayMethodsSortableError{
        .code = ArrayMethodsSortableErrorCode::DATES,
        .message = message,
    });
  }
  if (element->holdsObjects() || element->holdsResults()) {
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
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextArity::count(min, max, count);

    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::WRONG_ARITY,
        .message = message,
    });
  };
  auto textArity = [&](unsigned max) -> ResultArrayMethodCall {
    ResultElementsAreText text = ArrayMethods::textElements(element, name);
    if (text.isErr()) {
      this->argumentChecker->checkArgsLoosely(call);
      return err(ArrayMethodsCheckArrayMethodError{
          .code = ArrayMethodsCheckArrayMethodErrorCode::ELEMENTS_NOT_TEXT,
          .message = text.error().message,
      });
    }

    return arity(0, max);
  };
  auto elementArg = [&](size_t i) {
    call->args[i] =
        this->typeRelations->coerce(this->exprChecker->checkExpr(call->args[i], element), element);
  };
  auto positionArgs = [&](size_t from) {
    for (size_t i = from; i < count; ++i) {
      call->args[i] = this->memberChecker->checkPosition(call->args[i]);
    }
  };

  const ArrayMethodInfo *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextArrayMethod::noProperty(name, arrayType->name());

    return err(ArrayMethodsCheckArrayMethodError{
        .code = ArrayMethodsCheckArrayMethodErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }
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
    case ArrayMethod::EVERY: {
      Expr *checked = this->callbackChecker->checkCallbackMethod(call, member, expected);

      return ok(checked);
    }
    case ArrayMethod::KEYS:
    case ArrayMethod::VALUES:
    case ArrayMethod::ENTRIES: {
      IteratorMethods::ResultArrayIterateCall iterated =
          this->iteratorMethods->checkArrayIterator(call, member);
      if (iterated.isErr()) {
        return err(ArrayMethodsCheckArrayMethodError{
            .code = ArrayMethodsCheckArrayMethodErrorCode::WRONG_ARITY,
            .message = iterated.error().message,
        });
      }
      return ok(iterated.value());
    }
    case ArrayMethod::PUSH:
    case ArrayMethod::UNSHIFT: {
      bool spreads = call->hasSpread();
      for (size_t i = 0; i < count; ++i) {
        if (call->spreads(i)) {
          call->args[i] = this->typeRelations->coerce(
              this->exprChecker->checkExpr(call->args[i], arrayType), arrayType);
        } else {
          elementArg(i);
        }
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = Type::intTy();
      if (self && !spreads) {
        this->state.getFlowState().getFacts().shift(Term::lengthOf(self),
                                                    static_cast<int64_t>(count));
      }
      this->flowChecker->arrayResized(arrayType, true, false, spreads ? nullptr : self);
      return ok(call);
    }
    case ArrayMethod::POP:
    case ArrayMethod::SHIFT: {
      ResultArrayMethodCall counted = arity(0, 0);
      if (counted.isErr()) {
        return counted;
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      bool notEmpty = this->indexProofs->provesNotEmpty(member->object);
      call->mayBeOutOfRange = !notEmpty;
      call->type = notEmpty ? element : Type::optional(element);
      if (self) {
        if (notEmpty) {
          this->state.getFlowState().getFacts().shift(Term::lengthOf(self), -1);
        } else {
          this->state.getFlowState().getFacts().forgetIf(
              [&](const Facts::Fact &f) { return f.rhs == Term::lengthOf(self); });
        }
      }
      this->flowChecker->arrayResized(arrayType, false, true, self);
      return ok(call);
    }
    case ArrayMethod::AT: {
      ResultArrayMethodCall counted = arity(1, 1);
      if (counted.isErr()) {
        return counted;
      }
      positionArgs(0);
      call->target = BuiltinCall{.builtin = method->builtin};
      call->mayBeOutOfRange = !this->indexProofs->provesAtInBounds(member->object, call->args[0]);
      call->type = call->mayBeOutOfRange ? Type::optional(element) : element;
      return ok(call);
    }
    case ArrayMethod::SLICE: {
      ResultArrayMethodCall counted = arity(0, 2);
      if (counted.isErr()) {
        return counted;
      }
      positionArgs(0);
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = arrayType;
      return ok(call);
    }
    case ArrayMethod::CONCAT:
      for (size_t i = 0; i < count; ++i) {
        call->args[i] = this->exprChecker->checkExpr(call->args[i], arrayType);
        if (call->args[i]->type != arrayType) {
          call->args[i] = this->typeRelations->coerce(call->args[i], element);
        }
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = arrayType;
      return ok(call);
    case ArrayMethod::INDEX_OF:
    case ArrayMethod::LAST_INDEX_OF:
    case ArrayMethod::INCLUDES: {
      ResultArrayMethodCall counted = arity(1, 2);
      if (counted.isErr()) {
        return counted;
      }
      if (element->holdsResults()) {
        this->argumentChecker->checkArgsLoosely(call);
        std::string message = ErrorTextArrayMethod::results(name);

        return err(ArrayMethodsCheckArrayMethodError{
            .code = ArrayMethodsCheckArrayMethodErrorCode::RESULTS_COMPARED,
            .message = message,
        });
      }
      if (element->nonOptional()->isFunction()) {
        this->state.getInventoryStore().markComparesFunctions();
      }
      elementArg(0);
      positionArgs(1);
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = method->method == ArrayMethod::INCLUDES ? Type::boolTy() : Type::intTy();
      return ok(call);
    }
    case ArrayMethod::JOIN: {
      ResultArrayMethodCall counted = textArity(1);
      if (counted.isErr()) {
        return counted;
      }
      if (count == 1) {
        call->args[0] = this->typeRelations->coerce(
            this->exprChecker->checkExpr(call->args[0], Type::string()), Type::string());
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = Type::string();
      return ok(call);
    }
    case ArrayMethod::TO_STRING: {
      ResultArrayMethodCall counted = textArity(0);
      if (counted.isErr()) {
        return counted;
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = Type::string();
      return ok(call);
    }
    case ArrayMethod::REVERSE: {
      ResultArrayMethodCall counted = arity(0, 0);
      if (counted.isErr()) {
        return counted;
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = arrayType;
      return ok(call);
    }
    case ArrayMethod::FILL: {
      ResultArrayMethodCall counted = arity(1, 3);
      if (counted.isErr()) {
        return counted;
      }
      elementArg(0);
      positionArgs(1);
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = arrayType;
      return ok(call);
    }
    case ArrayMethod::SORT: {
      if (count != 0) {
        Expr *checked = this->callbackChecker->checkCallbackMethod(call, member, expected);

        return ok(checked);
      }
      ResultSortableElement ordered = ArrayMethods::sortable(element);
      if (ordered.isErr()) {
        return err(ArrayMethodsCheckArrayMethodError{
            .code = ArrayMethodsCheckArrayMethodErrorCode::NEEDS_COMPARISON,
            .message = ordered.error().message,
        });
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = arrayType;
      return ok(call);
    }
    case ArrayMethod::SPLICE: {
      ResultArrayMethodCall counted = arity(1, ~0u);
      if (counted.isErr()) {
        return counted;
      }
      for (size_t i = 0; i < count; ++i) {
        if (i < 2) {
          call->args[i] = this->memberChecker->checkPosition(call->args[i]);
        } else {
          elementArg(i);
        }
      }
      call->target = BuiltinCall{.builtin = method->builtin};
      call->type = arrayType;
      this->flowChecker->arrayResized(arrayType, true, true);
      return ok(call);
    }
  }
  llvm_unreachable("unknown array method");
}

}
