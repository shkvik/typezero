#include "compiler/semantic/bodies/objects/dict_methods/dict_methods.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
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
  this->callChecker = &all.callChecker;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->dictNewChecker = &all.dictNewChecker;
  this->argumentChecker = &all.argumentChecker;
  this->callbackChecker = &all.callbackChecker;
}

DictMethods::ResultDictMethodCalled DictMethods::checkDictMethod(CallExpr *call,
                                                                 MemberExpr *member) {
  const Type *type = member->object->type;
  auto text = DictCheckHelpers::dictText(member->object, type);
  call->type = Type::error();
  if (type->isRecord()) {
    return this->recordMethod(call, member, text);
  }

  bool isMap = type->isMap();
  const auto *found = DictMethodTable::find(member->name);
  bool isKnown = found != nullptr &&
                 (isMap ? DictMethodTable::isOnMap(*found) : DictMethodTable::isOnSet(*found));
  if (!isKnown) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextDictMethod::noProperty(member->name, type->name(), text, isMap);

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  DictCall dict{
      .call = call,
      .type = type,
      .key = type->keyType(),
      .value = type->valueType(),
      .builtin = DictMethodTable::getBuiltin(*found, isMap),
      .name = member->name,
      .isMap = isMap,
  };
  switch (found->method) {
    case DictMethod::GET:
      return this->checkGet(dict);
    case DictMethod::SET:
      return this->checkSet(dict);
    case DictMethod::ADD:
      return this->checkAdd(dict);
    case DictMethod::HAS:
    case DictMethod::DELETE_KEY:
      return this->checkKeyTest(dict);
    case DictMethod::CLEAR:
      return this->checkClear(dict);
    case DictMethod::KEYS:
    case DictMethod::VALUES:
    case DictMethod::ENTRIES:
      return this->checkIterating(dict, found->method);
    case DictMethod::FOR_EACH:
      return this->checkForEach(dict);
    case DictMethod::UNION:
    case DictMethod::INTERSECTION:
    case DictMethod::DIFFERENCE:
    case DictMethod::SYMMETRIC_DIFFERENCE:
    case DictMethod::IS_SUBSET_OF:
    case DictMethod::IS_SUPERSET_OF:
    case DictMethod::IS_DISJOINT_FROM:
      return this->checkSetCombine(dict, *found);
  }

  return ok(call);
}

DictMethods::ResultDictMethodCalled DictMethods::recordMethod(CallExpr *call, MemberExpr *member,
                                                              const std::string &text) {
  const Type *type = member->object->type;
  std::string key = text + "." + member->name.str();
  this->argumentChecker->walkArgsLoosely(call);
  auto inherited = ObjectMemberTable::find(member->name);
  bool hasOwn = inherited == ObjectMember::HAS_OWN_PROPERTY && call->args.size() == 1;
  if (hasOwn) {
    auto message = ErrorTextRecordMethod::hasOwn(key, text);

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::HAS_OWN_PROPERTY,
        .message = message,
    });
  }
  if (type->valueType()->isFunction()) {
    auto message = ErrorTextRecordMethod::undefined(key);

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  auto message = ErrorTextRecordMethod::noMethods(key, type->valueType()->name(), text);

  return err(DictMethodsCheckDictMethodError{
      .code = DictMethodsCheckDictMethodErrorCode::NO_RECORD_METHODS,
      .message = message,
  });
}

DictMethods::ResultDictMethodCalled DictMethods::checkDictArity(CallExpr *call, unsigned min,
                                                                unsigned max) {
  auto matches = this->argumentChecker->checkArity(call, min, max);
  if (matches.isErr()) {
    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::WRONG_ARITY,
        .message = matches.error().message,
    });
  }

  return ok(call);
}

DictMethods::ResultDictMethodCalled DictMethods::toBuiltinCall(CallExpr *call, Builtin builtin,
                                                               const Type *result) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return ok(call);
}

DictMethods::ResultDictMethodCalled DictMethods::checkGet(const DictCall &dict) {
  CallExpr *call = dict.call;
  auto counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);

  return this->toBuiltinCall(call, dict.builtin, Type::optional(dict.value));
}

DictMethods::ResultDictMethodCalled DictMethods::checkSet(const DictCall &dict) {
  CallExpr *call = dict.call;
  auto counted = this->checkDictArity(call, 2, 2);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);
  call->args[1] = this->argumentChecker->checkArg(call->args[1], dict.value);

  return this->toBuiltinCall(call, dict.builtin, dict.type);
}

DictMethods::ResultDictMethodCalled DictMethods::checkAdd(const DictCall &dict) {
  CallExpr *call = dict.call;
  auto counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);

  return this->toBuiltinCall(call, dict.builtin, dict.type);
}

DictMethods::ResultDictMethodCalled DictMethods::checkKeyTest(const DictCall &dict) {
  CallExpr *call = dict.call;
  auto counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);

  return this->toBuiltinCall(call, dict.builtin, Type::boolTy());
}

DictMethods::ResultDictMethodCalled DictMethods::checkClear(const DictCall &dict) {
  auto counted = this->checkDictArity(dict.call, 0, 0);
  if (counted.isErr()) {
    return counted;
  }

  return this->toBuiltinCall(dict.call, dict.builtin, Type::voidTy());
}

DictMethods::ResultDictMethodCalled DictMethods::checkIterating(const DictCall &dict,
                                                                DictMethod method) {
  auto counted = this->checkDictArity(dict.call, 0, 0);
  if (counted.isErr()) {
    return counted;
  }
  const auto *iterated = this->iteratedType(dict, method);

  return this->toBuiltinCall(dict.call, dict.builtin, iterated);
}

const Type *DictMethods::iteratedType(const DictCall &dict, DictMethod method) const {
  if (method == DictMethod::ENTRIES) {
    const Type *second = dict.isMap ? dict.value : dict.key;
    return Type::iterator(Type::tuple({dict.key, second}));
  }
  if (method == DictMethod::VALUES && dict.isMap) {
    return Type::iterator(dict.value);
  }

  return Type::iterator(dict.key);
}

DictMethods::ResultDictMethodCalled DictMethods::checkForEach(const DictCall &dict) {
  CallExpr *call = dict.call;
  bool isThisArg = call->args.size() == 2;
  if (isThisArg) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextDictForEach::thisArg();

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::FOR_EACH_THIS,
        .message = message,
    });
  }
  auto counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  Signature signature;
  signature.pad = false;
  signature.voidResult = true;
  signature.params = {dict.isMap ? dict.value : dict.key, dict.key, dict.type};
  int paramCount = dict.isMap ? 2 : 1;
  call->args[0] =
      this->callbackChecker->checkFunctionArg(call->args[0], signature, paramCount, dict.name);
  this->flowChecker->calledFunction();

  return this->toBuiltinCall(call, dict.builtin, Type::voidTy());
}

DictMethods::ResultDictMethodCalled DictMethods::checkSetCombine(const DictCall &dict,
                                                                 const DictMethodInfo &found) {
  CallExpr *call = dict.call;
  auto counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  auto *other = this->exprChecker->checkExpr(call->args[0], dict.type);
  call->args[0] = other;
  const Type *otherType = other->type;
  if (otherType->isError()) {
    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  bool isSet = otherType->isDict() && !otherType->isRecord() && otherType->keyType() == dict.key;
  if (!isSet) {
    auto message = ErrorTextSetCombine::notSet(dict.name, dict.key->name(), otherType->name());

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::NOT_A_SET,
        .message = message,
    });
  }
  bool isTesting = found.form.is<SetTestDictMethod>();

  return this->toBuiltinCall(call, dict.builtin, isTesting ? Type::boolTy() : dict.type);
}

DictMethods::ResultFromEntriesTyped DictMethods::checkFromEntries(CallExpr *call,
                                                                  const Type *expected) {
  if (this->callChecker->rejectSpread(call)) {
    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *type = wanted != nullptr && wanted->isRecord() ? wanted : nullptr;
  const Type *element =
      type != nullptr ? Type::tuple({Type::string(), type->valueType()}) : nullptr;
  const Type *key = nullptr;
  const Type *value = nullptr;
  bool isInferred = type == nullptr && this->dictNewChecker->inferPairs(call->args[0], key, value);
  if (isInferred && key->isError()) {
    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (isInferred) {
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
  if (type == nullptr) {
    return this->recordFromEntries(call, element);
  }
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = type;

  return ok(call);
}

DictMethods::ResultFromEntriesTyped DictMethods::recordFromEntries(CallExpr *call,
                                                                   const Type *element) {
  const Type *keys = element->fields()[0].type;
  if (!keys->isString()) {
    auto message = ErrorTextFromEntries::keys(keys->name());

    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::KEYS_NOT_STRINGS,
        .message = message,
    });
  }
  const Type *values = element->fields()[1].type;
  if (values->isUndefined()) {
    auto message = ErrorTextFromEntries::undefined();

    return err(DictMethodsCheckFromEntriesError{
        .code = DictMethodsCheckFromEntriesErrorCode::VALUES_UNDEFINED,
        .message = message,
    });
  }
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = Type::dict(Type::DictKind::Record, Type::string(), values);

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
  auto arity = this->argumentChecker->checkArity(call, 2, 2);
  if (arity.isErr()) {
    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  const Type *element = nullptr;
  auto what = ErrorTextFunctionName::quoted(fn);
  call->args[0] = this->dictNewChecker->checkDictSource(call->args[0], false, element, what);
  if (element->isError()) {
    call->args[1] = this->exprChecker->checkExpr(call->args[1]);

    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  Signature signature;
  signature.pad = false;
  signature.params = {element, Type::intTy()};
  call->args[1] = this->callbackChecker->checkFunctionArg(call->args[1], signature, 1, fn);
  this->flowChecker->calledFunction();
  const Type *callback = call->args[1]->type;
  if (!callback->isFunction()) {
    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  return this->groupByKey(call, element, map, fn);
}

DictMethods::ResultGroupByCallTyped DictMethods::groupByKey(CallExpr *call, const Type *element,
                                                            bool map, const std::string &fn) {
  const Type *key = call->args[1]->type->ret();
  if (key->isVoid() || key->isUndefined()) {
    auto message = ErrorTextGroupBy::noKey(fn);

    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::NO_GROUP_KEY,
        .message = message,
    });
  }
  if (!map && !key->isString()) {
    auto message = ErrorTextGroupBy::keys(key->name());

    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::KEYS_NOT_STRINGS,
        .message = message,
    });
  }
  auto allowed = this->dictChecker->checkKeyType(key);
  if (allowed.isErr()) {
    return err(DictMethodsCheckGroupByError{
        .code = DictMethodsCheckGroupByErrorCode::BAD_KEY_TYPE,
        .message = allowed.error().message,
    });
  }
  Type::DictKind kind = map ? Type::DictKind::Map : Type::DictKind::Record;
  call->target = BuiltinCall{.builtin = Builtin::DictGroupBy};
  call->type = Type::dict(kind, key, Type::array(element));

  return ok(call);
}

DictMethods::ResultMapFunctionTyped DictMethods::checkMapStatic(CallExpr *call,
                                                                MemberExpr *member) {
  auto found = GlobalMemberTable::find(GlobalName::MAP, member->name);
  if (found == GlobalMember::MAP_GROUP_BY) {
    auto grouped = this->checkGroupBy(call, true);
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
  this->argumentChecker->walkArgsLoosely(call);
  auto message = ErrorTextMapStatic::unknown(member->name);

  return err(DictMethodsCheckMapStaticError{
      .code = DictMethodsCheckMapStaticErrorCode::NOT_SUPPORTED,
      .message = message,
  });
}

DictMethods::ResultDictObjectCalled DictMethods::checkDictObjectCall(CallExpr *call,
                                                                     llvm::StringRef functionName) {
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (!type->isRecord()) {
    auto text = DictCheckHelpers::dictText(arg, type);
    llvm::StringRef kind = type->isMap() ? "Map" : "Set";
    auto objectMember = GlobalMemberTable::find(GlobalName::OBJECT, functionName);
    bool isSetEntries = objectMember == GlobalMember::OBJECT_ENTRIES && type->isSet();
    llvm::StringRef method =
        isSetEntries ? DictMethodTable::getName(DictMethod::VALUES) : functionName;
    auto message = ErrorTextDictObject::empty(functionName, kind, text, method);

    return err(DictMethodsCheckDictObjectCallError{
        .code = DictMethodsCheckDictObjectCallErrorCode::ALWAYS_EMPTY,
        .message = message,
    });
  }
  const Type *value = type->valueType();
  auto objectMember = GlobalMemberTable::find(GlobalName::OBJECT, functionName);
  bool hasKeys = objectMember == GlobalMember::OBJECT_KEYS;
  bool hasValues = objectMember == GlobalMember::OBJECT_VALUES;
  call->target = BuiltinCall{.builtin = hasKeys     ? Builtin::ObjectKeys
                                        : hasValues ? Builtin::ObjectValues
                                                    : Builtin::ObjectEntries};
  call->type = hasKeys     ? Type::array(Type::string())
               : hasValues ? Type::array(value)
                           : Type::array(Type::tuple({Type::string(), value}));

  return ok(call);
}

}
