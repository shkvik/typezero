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
  std::string text = DictCheckHelpers::dictText(member->object, type);
  call->type = Type::error();
  if (type->isRecord()) {
    return this->recordMethod(call, member, text);
  }

  bool isMap = type->isMap();
  const DictMethodInfo *found = DictMethodTable::find(member->name);
  bool known = found != nullptr &&
               (isMap ? DictMethodTable::isOnMap(*found) : DictMethodTable::isOnSet(*found));
  if (!known) {
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextDictMethod::noProperty(member->name, type->name(), text, isMap);

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
  this->argumentChecker->checkArgsLoosely(call);
  std::optional<ObjectMember> inherited = ObjectMemberTable::find(member->name);
  bool hasOwn = inherited == ObjectMember::HAS_OWN_PROPERTY && call->args.size() == 1;
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

DictMethods::ResultDictMethodCalled DictMethods::checkDictArity(CallExpr *call, unsigned min,
                                                                unsigned max) {
  ArgumentChecker::ResultArityMatches matches = this->argumentChecker->checkArity(call, min, max);
  if (matches.isErr()) {
    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::WRONG_ARITY,
        .message = matches.error().message,
    });
  }

  return ok(call);
}

DictMethods::ResultDictMethodCalled DictMethods::toBuiltinCall(CallExpr *call, Builtin builtin,
                                                               const Type *result) {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return ok(call);
}

DictMethods::ResultDictMethodCalled DictMethods::checkGet(const DictCall &dict) {
  CallExpr *call = dict.call;
  ResultDictMethodCalled counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);

  return DictMethods::toBuiltinCall(call, dict.builtin, Type::optional(dict.value));
}

DictMethods::ResultDictMethodCalled DictMethods::checkSet(const DictCall &dict) {
  CallExpr *call = dict.call;
  ResultDictMethodCalled counted = this->checkDictArity(call, 2, 2);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);
  call->args[1] = this->argumentChecker->checkArg(call->args[1], dict.value);

  return DictMethods::toBuiltinCall(call, dict.builtin, dict.type);
}

DictMethods::ResultDictMethodCalled DictMethods::checkAdd(const DictCall &dict) {
  CallExpr *call = dict.call;
  ResultDictMethodCalled counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);

  return DictMethods::toBuiltinCall(call, dict.builtin, dict.type);
}

DictMethods::ResultDictMethodCalled DictMethods::checkKeyTest(const DictCall &dict) {
  CallExpr *call = dict.call;
  ResultDictMethodCalled counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], dict.key);

  return DictMethods::toBuiltinCall(call, dict.builtin, Type::boolTy());
}

DictMethods::ResultDictMethodCalled DictMethods::checkClear(const DictCall &dict) {
  ResultDictMethodCalled counted = this->checkDictArity(dict.call, 0, 0);
  if (counted.isErr()) {
    return counted;
  }

  return DictMethods::toBuiltinCall(dict.call, dict.builtin, Type::voidTy());
}

DictMethods::ResultDictMethodCalled DictMethods::checkIterating(const DictCall &dict,
                                                                DictMethod method) {
  ResultDictMethodCalled counted = this->checkDictArity(dict.call, 0, 0);
  if (counted.isErr()) {
    return counted;
  }
  const Type *iterated = DictMethods::iteratedType(dict, method);

  return DictMethods::toBuiltinCall(dict.call, dict.builtin, iterated);
}

const Type *DictMethods::iteratedType(const DictCall &dict, DictMethod method) {
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
  bool thisArg = call->args.size() == 2;
  if (thisArg) {
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextDictForEach::thisArg();

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::FOR_EACH_THIS,
        .message = message,
    });
  }
  ResultDictMethodCalled counted = this->checkDictArity(call, 1, 1);
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

  return DictMethods::toBuiltinCall(call, dict.builtin, Type::voidTy());
}

DictMethods::ResultDictMethodCalled DictMethods::checkSetCombine(const DictCall &dict,
                                                                 const DictMethodInfo &found) {
  CallExpr *call = dict.call;
  ResultDictMethodCalled counted = this->checkDictArity(call, 1, 1);
  if (counted.isErr()) {
    return counted;
  }
  Expr *other = this->exprChecker->checkExpr(call->args[0], dict.type);
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
    std::string message =
        ErrorTextSetCombine::notSet(dict.name, dict.key->name(), otherType->name());

    return err(DictMethodsCheckDictMethodError{
        .code = DictMethodsCheckDictMethodErrorCode::NOT_A_SET,
        .message = message,
    });
  }
  bool tests = found.form.is<SetTestDictMethod>();

  return DictMethods::toBuiltinCall(call, dict.builtin, tests ? Type::boolTy() : dict.type);
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

  return this->groupByKey(call, element, map, fn);
}

DictMethods::ResultGroupByCallTyped DictMethods::groupByKey(CallExpr *call, const Type *element,
                                                            bool map, const std::string &fn) {
  const Type *key = call->args[1]->type->ret();
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
  Type::DictKind kind = map ? Type::DictKind::Map : Type::DictKind::Record;
  call->target = BuiltinCall{.builtin = Builtin::DictGroupBy};
  call->type = Type::dict(kind, key, Type::array(element));

  return ok(call);
}

DictMethods::ResultMapFunctionTyped DictMethods::checkMapStatic(CallExpr *call,
                                                                MemberExpr *member) {
  std::optional<GlobalMember> found = GlobalMemberTable::find(GlobalName::MAP, member->name);
  if (found == GlobalMember::MAP_GROUP_BY) {
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
    std::optional<GlobalMember> objectMember = GlobalMemberTable::find(GlobalName::OBJECT, fn);
    bool setEntries = objectMember == GlobalMember::OBJECT_ENTRIES && type->isSet();
    llvm::StringRef method = setEntries ? DictMethodTable::getName(DictMethod::VALUES) : fn;
    std::string message = ErrorTextDictObject::empty(fn, kind, text, method);

    return err(DictMethodsCheckDictObjectCallError{
        .code = DictMethodsCheckDictObjectCallErrorCode::ALWAYS_EMPTY,
        .message = message,
    });
  }
  const Type *value = type->valueType();
  std::optional<GlobalMember> objectMember = GlobalMemberTable::find(GlobalName::OBJECT, fn);
  bool keys = objectMember == GlobalMember::OBJECT_KEYS;
  bool values = objectMember == GlobalMember::OBJECT_VALUES;
  call->target = BuiltinCall{.builtin = keys     ? Builtin::ObjectKeys
                                        : values ? Builtin::ObjectValues
                                                 : Builtin::ObjectEntries};
  call->type = keys     ? Type::array(Type::string())
               : values ? Type::array(value)
                        : Type::array(Type::tuple({Type::string(), value}));

  return ok(call);
}

}
