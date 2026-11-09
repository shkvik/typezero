#include "compiler/semantic/bodies/objects/dict_methods/dict_methods.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

DictMethods::DictMethods(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.getCallChecker();
  this->dictChecker = &all.getDictChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->dictNewChecker = &all.getDictNewChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->callbackChecker = &all.getCallbackChecker();
}

DictMethods::ResultDictMethodCalled DictMethods::checkDictMethod(CallExpr *call,
                                                                 MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string text = DictCheckHelpers::dictText(member->object, type);
  call->type = Type::error();
  if (type->isRecord()) {
    std::string key = text + "." + name.str();
    this->argumentChecker->checkArgsLoosely(call);
    bool hasOwn = name == "hasOwnProperty" && call->args.size() == 1;
    if (hasOwn) {
      std::string message = ErrorTextRecordMethod::hasOwn(key, text);

      return err(DictMethodsCheckDictMethodError{
          .code = DictMethodsCheckDictMethodErrorCode::HAS_OWN_PROPERTY,
          .message = message,
      });
    }
    if (type->valueType()->isFunction()) {
      std::string message = ErrorTextRecordMethod::undefined(key);

      return err(DictMethodsCheckDictMethodError{
          .code = DictMethodsCheckDictMethodErrorCode::MAY_BE_UNDEFINED,
          .message = message,
      });
    }
    std::string message = ErrorTextRecordMethod::noMethods(key, type->valueType()->name(), text);

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::NO_RECORD_METHODS,
        .message = message,
    });
  }
  const Type *key = type->keyType();
  const Type *value = type->valueType();
  bool isMap = type->isMap();
  auto set = [&](Builtin builtin, const Type *result) -> ResultDictMethodCalled {
    call->target = BuiltinCall{.builtin = builtin};
    call->type = result;

    return ok(call);
  };
  auto arity = [&](unsigned min, unsigned max) -> ResultDictMethodCalled {
    ArgumentChecker::ResultArityMatches matches = this->argumentChecker->checkArity(call, min, max);
    if (matches.isErr()) {
      return err(DictMethodsCheckDictMethodError{
          .code = DictMethodsCheckDictMethodErrorCode::WRONG_ARITY,
          .message = matches.error().message,
      });
    }

    return ok(call);
  };
  auto keyArg = [&](size_t i) {
    call->args[i] = this->argumentChecker->checkArg(call->args[i], key);
  };

  if (name == "get" && isMap) {
    ResultDictMethodCalled counted = arity(1, 1);
    if (counted.isErr()) {
      return counted;
    }
    keyArg(0);
    return set(Builtin::MapGet, Type::optional(value));
  }
  if (name == "set" && isMap) {
    ResultDictMethodCalled counted = arity(2, 2);
    if (counted.isErr()) {
      return counted;
    }
    keyArg(0);
    call->args[1] = this->argumentChecker->checkArg(call->args[1], value);
    return set(Builtin::MapSet, type);
  }
  if (name == "add" && !isMap) {
    ResultDictMethodCalled counted = arity(1, 1);
    if (counted.isErr()) {
      return counted;
    }
    keyArg(0);
    return set(Builtin::SetAdd, type);
  }
  if (name == "has" || name == "delete") {
    ResultDictMethodCalled counted = arity(1, 1);
    if (counted.isErr()) {
      return counted;
    }
    keyArg(0);
    return set(name == "has" ? Builtin::MapHas : Builtin::MapDelete, Type::boolTy());
  }
  if (name == "clear") {
    ResultDictMethodCalled counted = arity(0, 0);
    if (counted.isErr()) {
      return counted;
    }
    return set(Builtin::MapClear, Type::voidTy());
  }
  if (name == "keys" || name == "values" || name == "entries") {
    ResultDictMethodCalled counted = arity(0, 0);
    if (counted.isErr()) {
      return counted;
    }
    if (!isMap) {
      return name == "entries" ? set(Builtin::SetEntries, Type::iterator(Type::tuple({key, key})))
                               : set(Builtin::SetValues, Type::iterator(key));
    }
    return name == "keys"     ? set(Builtin::MapKeys, Type::iterator(key))
           : name == "values" ? set(Builtin::MapValues, Type::iterator(value))
                              : set(Builtin::MapEntries, Type::iterator(Type::tuple({key, value})));
  }
  if (name == "forEach") {
    bool thisArg = call->args.size() == 2;
    if (thisArg) {
      this->argumentChecker->checkArgsLoosely(call);
      std::string message = ErrorTextDictForEach::thisArg();

      return err(DictMethodsCheckDictMethodError{
          .code = DictMethodsCheckDictMethodErrorCode::FOR_EACH_THIS,
          .message = message,
      });
    }
    ResultDictMethodCalled counted = arity(1, 1);
    if (counted.isErr()) {
      return counted;
    }
    Signature sig;
    sig.pad = false;
    sig.voidResult = true;
    sig.params = {isMap ? value : key, key, type};
    call->args[0] =
        this->callbackChecker->checkFunctionArg(call->args[0], sig, isMap ? 2 : 1, "forEach");
    this->flowChecker->calledFunction();
    return set(Builtin::MapForEach, Type::voidTy());
  }
  struct Combine {
    llvm::StringLiteral name;
    Builtin builtin;
    bool test;
  };
  static const Combine combines[] = {
      {"union", Builtin::SetUnion, false},
      {"intersection", Builtin::SetIntersection, false},
      {"difference", Builtin::SetDifference, false},
      {"symmetricDifference", Builtin::SetSymmetricDifference, false},
      {"isSubsetOf", Builtin::SetIsSubsetOf, true},
      {"isSupersetOf", Builtin::SetIsSupersetOf, true},
      {"isDisjointFrom", Builtin::SetIsDisjointFrom, true},
  };
  for (const Combine &c : combines) {
    if (isMap || c.name != name) {
      continue;
    }
    ResultDictMethodCalled counted = arity(1, 1);
    if (counted.isErr()) {
      return counted;
    }
    Expr *&other = call->args[0];
    other = this->exprChecker->checkExpr(other, type);
    const Type *otherType = other->type;
    if (otherType->isError()) {
      return err(DictMethodsCheckDictMethodError{
          .code = DictMethodsCheckDictMethodErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
    bool isSet = otherType->isDict() && !otherType->isRecord() && otherType->keyType() == key;
    if (!isSet) {
      std::string message = ErrorTextSetCombine::notSet(name, key->name(), otherType->name());

      return err(DictMethodsCheckDictMethodError{
          .code = DictMethodsCheckDictMethodErrorCode::NOT_A_SET,
          .message = message,
      });
    }
    return set(c.builtin, c.test ? Type::boolTy() : type);
  }
  this->argumentChecker->checkArgsLoosely(call);
  std::string message = ErrorTextDictMethod::noProperty(name, type->name(), text, isMap);

  return err(DictMethodsCheckDictMethodError{
      .code = DictMethodsCheckDictMethodErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

DictMethods::ResultFromEntriesTyped DictMethods::checkFromEntries(CallExpr *call,
                                                                  const Type *expected) {
  if (this->callChecker->rejectSpread(call)) {
    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  const Type *type = wanted && wanted->isRecord() ? wanted : nullptr;
  const Type *element = type ? Type::tuple({Type::string(), type->valueType()}) : nullptr;
  const Type *key = nullptr;
  const Type *value = nullptr;
  bool inferred = !type && this->dictNewChecker->inferPairs(call->args[0], key, value);
  if (inferred && key->isError()) {
    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (inferred) {
    element = call->args[0]->type->inner();
  } else {
    call->args[0] =
        this->dictNewChecker->checkDictSource(call->args[0], true, element, "'Object.fromEntries'");
  }
  if (element->isError()) {
    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type) {
    const Type *keys = element->fields()[0].type;
    if (!keys->isString()) {
      std::string message = ErrorTextFromEntries::keys(keys->name());

      return err(DictMethodsCheckFromEntriesError{
          .code = DictMethodsCheckFromEntriesErrorCode::KEYS_NOT_STRINGS,
          .message = message,
      });
    }
    const Type *values = element->fields()[1].type;
    if (values->isUndefined()) {
      std::string message = ErrorTextFromEntries::undefined();

      return err(DictMethodsCheckFromEntriesError{
          .code = DictMethodsCheckFromEntriesErrorCode::VALUES_UNDEFINED,
          .message = message,
      });
    }
    type = Type::dict(Type::DictKind::Record, Type::string(), values);
  }
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = type;

  return ok(call);
}

DictMethods::ResultGroupByCallTyped DictMethods::checkGroupBy(CallExpr *call, bool map) {
  std::string fn = map ? "Map.groupBy" : "Object.groupBy";
  if (this->callChecker->rejectSpread(call)) {
    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  const Type *element = nullptr;
  std::string what = ErrorTextFunctionName::quoted(fn);
  call->args[0] = this->dictNewChecker->checkDictSource(call->args[0], false, element, what);
  if (element->isError()) {
    call->args[1] = this->exprChecker->checkExpr(call->args[1]);

    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  Signature sig;
  sig.pad = false;
  sig.params = {element, Type::intTy()};
  call->args[1] = this->callbackChecker->checkFunctionArg(call->args[1], sig, 1, fn);
  this->flowChecker->calledFunction();
  const Type *callback = call->args[1]->type;
  if (!callback->isFunction()) {
    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  const Type *key = callback->ret();
  if (key->isVoid() || key->isUndefined()) {
    std::string message = ErrorTextGroupBy::noKey(fn);

    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::NO_GROUP_KEY,
        .message = message,
    });
  }
  if (!map && !key->isString()) {
    std::string message = ErrorTextGroupBy::keys(key->name());

    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::KEYS_NOT_STRINGS,
        .message = message,
    });
  }
  DictChecker::ResultKeyTypeIsAllowed allowed = this->dictChecker->checkKeyType(key);
  if (allowed.isErr()) {
    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::BAD_KEY_TYPE,
        .message = allowed.error().message,
    });
  }
  call->target = BuiltinCall{.builtin = Builtin::DictGroupBy};
  call->type =
      Type::dict(map ? Type::DictKind::Map : Type::DictKind::Record, key, Type::array(element));

  return ok(call);
}

DictMethods::ResultMapFunctionTyped DictMethods::checkMapStatic(CallExpr *call,
                                                                MemberExpr *member) {
  if (member->name == "groupBy") {
    ResultGroupByCallTyped grouped = this->checkGroupBy(call, true);
    if (grouped.isOk()) {
      return ok(grouped.value());
    }
    DictMethodsCheckMapStaticErrorCode code = DictMethodsCheckMapStaticErrorCode::ALREADY_REPORTED;
    switch (grouped.error().code) {
      case DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED:
        code = DictMethodsCheckMapStaticErrorCode::ALREADY_REPORTED;
        break;
      case DictMethodsCheckGroupByErrorCode::WRONG_ARITY:
        code = DictMethodsCheckMapStaticErrorCode::WRONG_ARITY;
        break;
      case DictMethodsCheckGroupByErrorCode::NO_GROUP_KEY:
      case DictMethodsCheckGroupByErrorCode::KEYS_NOT_STRINGS:
      case DictMethodsCheckGroupByErrorCode::BAD_KEY_TYPE:
        code = DictMethodsCheckMapStaticErrorCode::WRONG_GROUP_KEY;
        break;
    }

    return err(DictMethodsCheckMapStaticError{
        .code = code,
        .message = grouped.error().message,
    });
  }
  this->argumentChecker->checkArgsLoosely(call);
  std::string message = ErrorTextMapStatic::unknown(member->name);

  return err(DictMethodsCheckMapStaticError{
      .code = DictMethodsCheckMapStaticErrorCode::NOT_SUPPORTED,
      .message = message,
  });
}

DictMethods::ResultDictObjectCalled DictMethods::checkDictObjectCall(CallExpr *call,
                                                                     llvm::StringRef fn) {
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (!type->isRecord()) {
    std::string text = DictCheckHelpers::dictText(arg, type);
    llvm::StringRef kind = type->isMap() ? "Map" : "Set";
    llvm::StringRef method = fn == "entries" && type->isSet() ? llvm::StringRef("values") : fn;
    std::string message = ErrorTextDictObject::empty(fn, kind, text, method);

    return err(DictMethodsCheckDictObjectCallError{
        .code = DictMethodsCheckDictObjectCallErrorCode::ALWAYS_EMPTY,
        .message = message,
    });
  }
  const Type *value = type->valueType();
  call->target = BuiltinCall{.builtin = fn == "keys"     ? Builtin::ObjectKeys
                                        : fn == "values" ? Builtin::ObjectValues
                                                         : Builtin::ObjectEntries};
  call->type = fn == "keys"     ? Type::array(Type::string())
               : fn == "values" ? Type::array(value)
                                : Type::array(Type::tuple({Type::string(), value}));

  return ok(call);
}

}
