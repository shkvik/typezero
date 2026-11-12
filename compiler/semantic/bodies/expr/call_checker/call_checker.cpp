#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/function/function_method_table/function_method_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/semantic/bodies/classes/inherit_checker/inherit_checker.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/name_checker/name_checker.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/semantic/bodies/functions/function_method_checker/function_method_checker.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_methods/dict_methods.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/bodies/values/array_methods/array_methods.h"
#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker.h"
#include "compiler/semantic/bodies/values/number_methods/number_methods.h"
#include "compiler/semantic/bodies/values/string_methods/string_methods.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/library/async/promise_checker/promise_checker.h"
#include "compiler/semantic/library/async/promise_static_checker/promise_static_checker.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/console/console_checker/console_checker.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/semantic/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/intl/intl_method_checker/intl_method_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/json/json_static_checker/json_static_checker.h"
#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/result/result_method_checker/result_method_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/semantic/library/tz_modules/http_checker/http_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/library/tz_modules/url_checker/url_checker.h"
#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_checker/union_checker.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr const char *kThisName = "this";
constexpr const char *kSuperName = "super";
constexpr const char *kIteratorMethod = "[Symbol.iterator]";
constexpr const char *kAsyncIteratorMethod = "[Symbol.asyncIterator]";

constexpr GlobalName kTypeArgFreeGlobals[] = {
    GlobalName::STRING,  GlobalName::NUMBER,  GlobalName::OBJECT, GlobalName::ITERATOR,
    GlobalName::ARRAY,   GlobalName::MAP,     GlobalName::SYMBOL, GlobalName::RESULT,
    GlobalName::CONSOLE, GlobalName::PROCESS, GlobalName::MATH,   GlobalName::BIT,
    GlobalName::REG_EXP, GlobalName::DATE,    GlobalName::INTL,   GlobalName::DECIMAL};

}

CallChecker::CallChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->newChecker = &all.newChecker;
  this->urlChecker = &all.urlChecker;
  this->dateChecker = &all.dateChecker;
  this->dictMethods = &all.dictMethods;
  this->exprChecker = &all.exprChecker;
  this->httpChecker = &all.httpChecker;
  this->intlChecker = &all.intlChecker;
  this->jsonChecker = &all.jsonChecker;
  this->mathChecker = &all.mathChecker;
  this->nameChecker = &all.nameChecker;
  this->arrayMethods = &all.arrayMethods;
  this->bytesChecker = &all.bytesChecker;
  this->libraryCalls = &all.libraryCalls;
  this->matchChecker = &all.matchChecker;
  this->regexChecker = &all.regexChecker;
  this->timerChecker = &all.timerChecker;
  this->tupleChecker = &all.tupleChecker;
  this->unionChecker = &all.unionChecker;
  this->concatChecker = &all.concatChecker;
  this->cryptoChecker = &all.cryptoChecker;
  this->numberMethods = &all.numberMethods;
  this->objectChecker = &all.objectChecker;
  this->resultChecker = &all.resultChecker;
  this->staticChecker = &all.staticChecker;
  this->stringMethods = &all.stringMethods;
  this->typeRelations = &all.typeRelations;
  this->argListChecker = &all.argListChecker;
  this->consoleChecker = &all.consoleChecker;
  this->decimalChecker = &all.decimalChecker;
  this->genericChecker = &all.genericChecker;
  this->inheritChecker = &all.inheritChecker;
  this->processChecker = &all.processChecker;
  this->promiseChecker = &all.promiseChecker;
  this->streamPipeline = &all.streamPipeline;
  this->unknownChecker = &all.unknownChecker;
  this->weakRefChecker = &all.weakRefChecker;
  this->argumentChecker = &all.argumentChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->nestedFunctions = &all.nestedFunctions;
  this->topLevelChecker = &all.topLevelChecker;
  this->typeArgInferrer = &all.typeArgInferrer;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->webCryptoChecker = &all.webCryptoChecker;
  this->dateFormatChecker = &all.dateFormatChecker;
  this->intlMethodChecker = &all.intlMethodChecker;
  this->jsonStaticChecker = &all.jsonStaticChecker;
  this->methodCallChecker = &all.methodCallChecker;
  this->subtleCallChecker = &all.subtleCallChecker;
  this->eventTargetChecker = &all.eventTargetChecker;
  this->reflectCallChecker = &all.reflectCallChecker;
  this->bufferStaticChecker = &all.bufferStaticChecker;
  this->cryptoMethodChecker = &all.cryptoMethodChecker;
  this->resultMethodChecker = &all.resultMethodChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
  this->promiseStaticChecker = &all.promiseStaticChecker;
  this->runtimeObjectChecker = &all.runtimeObjectChecker;
  this->functionMethodChecker = &all.functionMethodChecker;
  this->globalFunctionChecker = &all.globalFunctionChecker;
}

CallChecker::ResultTypeArgsAbsent CallChecker::checkNoTypeArgs(const MemberExpr *member) const {
  if (!member->typeArgs.empty()) {
    auto message = ErrorTextCallTypeArgs::forGeneric();

    return err(CallCheckerNoTypeArgsError{
        .code = CallCheckerNoTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(member);
}

CallChecker::ResultCalleeCallable
CallChecker::checkCalleeCallable(const Type *type, const NameExpr *calleeName) const {
  bool isOptional = type->isOptional() && type->inner()->isFunction();
  if (isOptional) {
    auto message = ErrorTextCallee::undefined();

    return err(CallCheckerCalleeCallableError{
        .code = CallCheckerCalleeCallableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isError()) {
    return err(CallCheckerCalleeCallableError{
        .code = CallCheckerCalleeCallableErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type->isFunction() && calleeName != nullptr) {
    auto message = ErrorTextCallee::notFunc(calleeName->name, type->name());

    return err(CallCheckerCalleeCallableError{
        .code = CallCheckerCalleeCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (!type->isFunction()) {
    auto message = ErrorTextCallee::uncallable(type->name());

    return err(CallCheckerCalleeCallableError{
        .code = CallCheckerCalleeCallableErrorCode::NOT_CALLABLE,
        .message = message,
    });
  }

  return ok(type);
}

Expr *CallChecker::checkCall(CallExpr *call, const Type *expected) {
  call->type = Type::error();
  this->argListChecker->expandListSpreads(call->args, call->spread);
  this->rewriteSymbolCallee(call);
  auto *superName = dyn_cast<NameExpr>(call->callee);
  if (superName != nullptr && superName->name == kSuperName) {
    return this->inheritChecker->checkSuperCall(call);
  }
  auto *nodeCall = this->builtinModuleChecker->checkNodeCall(call, expected);
  if (nodeCall != nullptr) {
    return nodeCall;
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member != nullptr && !call->optional) {
    return this->checkMemberCall(call, member, expected);
  }
  auto *name = dyn_cast<NameExpr>(call->callee);
  bool isNamedCall = name != nullptr && !call->optional;
  auto *named = isNamedCall ? this->checkNamedCall(call, name, expected) : nullptr;
  if (named != nullptr) {
    return named;
  }

  return this->checkCalleeValueCall(call);
}

void CallChecker::rewriteSymbolCallee(CallExpr *call) {
  auto *index = dyn_cast<IndexExpr>(call->callee);
  if (index == nullptr || index->optional) {
    return;
  }
  auto *key = dyn_cast<MemberExpr>(index->index);
  bool isIterator = key != nullptr && this->isGlobalMember(key, GlobalMember::SYMBOL_ITERATOR);
  bool isAsyncIterator =
      key != nullptr && this->isGlobalMember(key, GlobalMember::SYMBOL_ASYNC_ITERATOR);
  if (!isIterator && !isAsyncIterator) {
    return;
  }
  llvm::StringRef name = isIterator ? kIteratorMethod : kAsyncIteratorMethod;
  call->callee = this->state.getAst().make<MemberExpr>(index->loc(), index->object, name);
}

void CallChecker::rejectTypeArgs(MemberExpr *member) {
  auto absent = this->checkNoTypeArgs(member);
  if (absent.isErr()) {
    this->state.getDiagnostics().error(member->loc(), absent.error().message);
    member->typeArgs.clear();
  }
}

void CallChecker::rejectGlobalTypeArgs(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr) {
    return;
  }
  auto entry = this->scopes->lookup(object->name);
  if (entry.cls() != nullptr) {
    return;
  }
  for (GlobalName global : kTypeArgFreeGlobals) {
    if (this->isGlobalName(object, global)) {
      return this->rejectTypeArgs(member);
    }
  }
}

Expr *CallChecker::checkMemberCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (this->isGlobalName(member->object, GlobalName::PROMISE)) {
    return this->promiseStaticChecker->checkPromiseStatic(call, member, expected);
  }
  this->rejectGlobalTypeArgs(member);
  auto *superObject = dyn_cast<NameExpr>(member->object);
  if (superObject != nullptr && superObject->name == kSuperName) {
    return this->inheritChecker->checkSuperMethodCall(call, member);
  }
  auto *language = this->checkLanguageStaticCall(call, member, expected);
  if (language != nullptr) {
    return language;
  }
  auto *library = this->checkLibraryStaticCall(call, member, expected);
  if (library != nullptr) {
    return library;
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  ClassDecl *classDecl = object != nullptr ? this->scopes->lookup(object->name).cls() : nullptr;
  if (classDecl != nullptr) {
    return this->staticChecker->checkClassStaticCall(call, member, classDecl, expected);
  }
  if (object != nullptr && object->name == kThisName) {
    this->state.getClassContext().rememberThisMember(member->name);
  }
  auto *collection = this->checkCollectionStaticCall(call, member);
  if (collection != nullptr) {
    return collection;
  }
  const auto *iteratorMethod = IteratorMethodTable::find(member->name);
  if (iteratorMethod != nullptr && iteratorMethod->form.is<IteratorConsumerMethod>()) {
    this->iteratorMethods->markFused(member->object);
  }
  auto *checked = this->exprChecker->checkExpr(member->object);
  member->object = this->genericChecker->apparent(checked);
  this->state.getClassContext().forgetThisMember();
  if (member->optional) {
    member->object = this->exprChecker->chainLink(member->object);
  }

  return this->checkReceiverCall(call, member, expected);
}

Expr *CallChecker::checkLanguageStaticCall(CallExpr *call, MemberExpr *member,
                                           const Type *expected) {
  auto *resultStatic = this->resultChecker->checkResultStatic(call, member);
  if (resultStatic != nullptr) {
    return resultStatic;
  }
  if (this->isGlobalName(member->object, GlobalName::JSON)) {
    return this->jsonStaticChecker->checkJsonStatic(call, member);
  }
  if (this->isGlobalMember(member, GlobalMember::ARRAY_IS_ARRAY)) {
    return this->jsonChecker->checkArrayIsArray(call, member);
  }
  if (this->isGlobalMember(member, GlobalMember::JSON_IS_JSON)) {
    return this->jsonChecker->checkJsonIsJson(call, member);
  }
  if (this->isGlobalName(member->object, GlobalName::REFLECT)) {
    return this->reflectCallChecker->checkReflectCall(call, member, expected);
  }
  auto *consoleCall = this->consoleChecker->checkConsoleCall(call, member);
  if (consoleCall != nullptr) {
    return consoleCall;
  }
  auto *processCall = this->processChecker->checkProcessCall(call, member);
  if (processCall != nullptr) {
    return processCall;
  }
  auto *nested = dyn_cast<MemberExpr>(member->object);
  bool isBitCall = this->isGlobalName(member->object, GlobalName::BIT) ||
                   (nested != nullptr && this->isGlobalName(nested->object, GlobalName::BIT));
  if (isBitCall) {
    return this->mathChecker->checkBitCall(call, member);
  }
  if (this->isGlobalName(member->object, GlobalName::MATH)) {
    return this->mathChecker->checkMathCall(call, member, expected);
  }
  if (this->isGlobalName(member->object, GlobalName::REG_EXP)) {
    return this->regexChecker->checkRegExpStatic(call, member);
  }

  return nullptr;
}

Expr *CallChecker::checkLibraryStaticCall(CallExpr *call, MemberExpr *member,
                                          const Type *expected) {
  if (this->isGlobalName(member->object, GlobalName::INTL)) {
    return this->dateFormatChecker->checkIntlCall(call, member);
  }
  auto *nested = dyn_cast<MemberExpr>(member->object);
  bool isIntlMember = nested != nullptr && this->isGlobalName(nested->object, GlobalName::INTL);
  bool isLocale = isIntlMember && LibraryClassTable::isName(nested->name, LibraryClass::LOCALE);
  if (isLocale) {
    return this->intlChecker->checkLocaleStatic(call, member, expected);
  }
  if (isIntlMember) {
    return this->intlChecker->checkIntlStatic(call, member);
  }
  if (this->isGlobalName(member->object, GlobalName::DECIMAL)) {
    return this->decimalChecker->checkDecimalStatic(call, member);
  }
  if (this->cryptoChecker->isX509Class(member->object)) {
    return this->cryptoChecker->checkX509Parse(call, member);
  }
  if (this->isGlobalName(member->object, GlobalName::CRYPTO)) {
    return this->cryptoChecker->checkWebCryptoCall(call, member);
  }
  auto *bytesStatic = this->bufferStaticChecker->checkBytesStatic(call, member);
  if (bytesStatic != nullptr) {
    return bytesStatic;
  }
  bool isNumberStatic = this->isGlobalName(member->object, GlobalName::STRING) ||
                        this->isGlobalName(member->object, GlobalName::NUMBER);
  if (isNumberStatic) {
    return this->checkNumberStaticCall(call, member);
  }
  if (this->isGlobalName(member->object, GlobalName::OBJECT)) {
    return this->checkObjectStaticCall(call, member, expected);
  }

  return nullptr;
}

Expr *CallChecker::checkNumberStaticCall(CallExpr *call, MemberExpr *member) {
  if (this->rejectSpread(call)) {
    return call;
  }
  auto made = this->numberMethods->checkStaticCall(call, member);
  if (made.isOk()) {
    return made.value();
  }
  if (made.error().code != NumberMethodsCheckStaticCallErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), made.error().message);
  }

  return call;
}

Expr *CallChecker::checkObjectStaticCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  auto objectCall = this->objectChecker->checkObjectCall(call, member, expected);
  if (objectCall.isOk()) {
    return objectCall.value();
  }
  auto code = objectCall.error().code;
  if (code == ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED) {
    return call;
  }
  auto at = this->objectCallErrorPlace(call, member, code);
  this->state.getDiagnostics().error(at, objectCall.error().message);

  return call;
}

llvm::SMLoc CallChecker::objectCallErrorPlace(const CallExpr *call, const MemberExpr *member,
                                              ObjectCheckerCheckObjectCallErrorCode code) const {
  switch (code) {
    case ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED:
    case ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_ARITY:
    case ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_ARITY:
    case ObjectCheckerCheckObjectCallErrorCode::NOT_SUPPORTED:
    case ObjectCheckerCheckObjectCallErrorCode::WRONG_ARITY:
      return member->loc();
    case ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_KEY:
      return call->args[1]->loc();
    case ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_PAIRS:
    case ObjectCheckerCheckObjectCallErrorCode::MAY_BE_UNDEFINED:
    case ObjectCheckerCheckObjectCallErrorCode::DICT_ALWAYS_EMPTY:
    case ObjectCheckerCheckObjectCallErrorCode::NOT_AN_OBJECT:
    case ObjectCheckerCheckObjectCallErrorCode::EMITTER_FIELDS:
    case ObjectCheckerCheckObjectCallErrorCode::METHOD_FIELDS:
    case ObjectCheckerCheckObjectCallErrorCode::NO_COMMON_TYPE:
    case ObjectCheckerCheckObjectCallErrorCode::NO_FIELDS:
    case ObjectCheckerCheckObjectCallErrorCode::NUMERIC_ENUM:
      return this->firstArgumentPlace(call, member);
  }

  return member->loc();
}

llvm::SMLoc CallChecker::firstArgumentPlace(const CallExpr *call, const MemberExpr *member) const {
  if (call->args.empty()) {
    return member->loc();
  }

  return call->args[0]->loc();
}

Expr *CallChecker::checkCollectionStaticCall(CallExpr *call, MemberExpr *member) {
  if (this->isGlobalName(member->object, GlobalName::ITERATOR)) {
    return this->checkIteratorFromCall(call, member);
  }
  if (this->isGlobalName(member->object, GlobalName::ARRAY)) {
    return this->iteratorMethods->checkArrayFrom(call, member);
  }
  if (this->isGlobalName(member->object, GlobalName::MAP)) {
    return this->checkMapStaticCall(call, member);
  }

  return nullptr;
}

Expr *CallChecker::checkIteratorFromCall(CallExpr *call, MemberExpr *member) {
  auto from = this->iteratorMethods->checkIteratorFrom(call, member);
  if (from.isOk()) {
    return from.value();
  }
  auto code = from.error().code;
  if (code == IteratorMethodsCheckIteratorFromErrorCode::ALREADY_REPORTED) {
    return call;
  }
  auto at = this->iteratorFromErrorPlace(call, member, code);
  this->state.getDiagnostics().error(at, from.error().message);

  return call;
}

llvm::SMLoc
CallChecker::iteratorFromErrorPlace(const CallExpr *call, const MemberExpr *member,
                                    IteratorMethodsCheckIteratorFromErrorCode code) const {
  switch (code) {
    case IteratorMethodsCheckIteratorFromErrorCode::NOT_ITERABLE:
      return call->args[0]->loc();
    case IteratorMethodsCheckIteratorFromErrorCode::ALREADY_REPORTED:
    case IteratorMethodsCheckIteratorFromErrorCode::NOT_SUPPORTED:
    case IteratorMethodsCheckIteratorFromErrorCode::WRONG_ARITY:
      return member->loc();
  }

  return member->loc();
}

Expr *CallChecker::checkMapStaticCall(CallExpr *call, MemberExpr *member) {
  auto mapped = this->dictMethods->checkMapStatic(call, member);
  if (mapped.isOk()) {
    return mapped.value();
  }
  auto code = mapped.error().code;
  if (code == DictMethodsCheckMapStaticErrorCode::ALREADY_REPORTED) {
    return call;
  }
  auto at = this->mapStaticErrorPlace(call, member, code);
  this->state.getDiagnostics().error(at, mapped.error().message);

  return call;
}

llvm::SMLoc CallChecker::mapStaticErrorPlace(const CallExpr *call, const MemberExpr *member,
                                             DictMethodsCheckMapStaticErrorCode code) const {
  switch (code) {
    case DictMethodsCheckMapStaticErrorCode::WRONG_GROUP_KEY:
      return call->args[1]->loc();
    case DictMethodsCheckMapStaticErrorCode::ALREADY_REPORTED:
    case DictMethodsCheckMapStaticErrorCode::WRONG_ARITY:
    case DictMethodsCheckMapStaticErrorCode::NOT_SUPPORTED:
      return member->loc();
  }

  return member->loc();
}

Expr *CallChecker::checkReceiverCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *receiver = member->object->type;
  if (!receiver->isClass() && !receiver->isUnion()) {
    this->rejectTypeArgs(member);
  }
  auto *library = this->checkLibraryReceiverCall(call, member, expected);
  if (library != nullptr) {
    return library;
  }
  auto unknown = this->checkUnknownReceiverCall(call, member);
  if (unknown.has_value()) {
    return *unknown;
  }
  auto *callable = this->checkCallableReceiverCall(call, member, expected);
  if (callable != nullptr) {
    return callable;
  }

  return this->checkShapedReceiverCall(call, member, expected);
}

std::optional<Expr *> CallChecker::checkUnknownReceiverCall(CallExpr *call, MemberExpr *member) {
  auto unknown = this->unknownChecker->checkUnknownMethod(call, member);
  if (unknown.isOk()) {
    return unknown.value();
  }
  auto code = unknown.error().code;
  if (code == UnknownCheckerCheckUnknownMethodErrorCode::NOT_UNKNOWN) {
    return std::nullopt;
  }
  auto at = this->unknownMethodErrorPlace(member, code);
  this->state.getDiagnostics().error(at, unknown.error().message);

  return call;
}

llvm::SMLoc
CallChecker::unknownMethodErrorPlace(const MemberExpr *member,
                                     UnknownCheckerCheckUnknownMethodErrorCode code) const {
  switch (code) {
    case UnknownCheckerCheckUnknownMethodErrorCode::NOT_NARROWED:
      return member->object->loc();
    case UnknownCheckerCheckUnknownMethodErrorCode::NOT_UNKNOWN:
    case UnknownCheckerCheckUnknownMethodErrorCode::RUNTIME_CLASS:
    case UnknownCheckerCheckUnknownMethodErrorCode::CLASS_VALUE:
      return member->loc();
  }

  return member->loc();
}

Expr *CallChecker::checkLibraryReceiverCall(CallExpr *call, MemberExpr *member,
                                            const Type *expected) {
  const Type *receiver = member->object->type;
  if (receiver->isRuntimeObject()) {
    return this->runtimeObjectChecker->checkRuntimeMethod(call, member, expected);
  }
  auto *resultMethod = receiver->isResult()
                           ? this->resultMethodChecker->checkResultMethod(call, member, expected)
                           : nullptr;
  if (resultMethod != nullptr) {
    return resultMethod;
  }
  if (receiver->nonOptional()->isJson()) {
    return this->jsonChecker->reportJsonMethod(call, member);
  }
  if (receiver->isRegExp()) {
    return this->regexChecker->checkRegexMethod(call, member);
  }
  auto *dateMethod =
      receiver->isDate() ? this->dateChecker->checkDateMethod(call, member) : nullptr;
  if (dateMethod != nullptr) {
    return dateMethod;
  }
  if (receiver->isDateFormat()) {
    return this->dateFormatChecker->checkDateFormatMethod(call, member);
  }
  if (receiver->isIntl()) {
    return this->intlMethodChecker->checkIntlMethod(call, member);
  }
  if (receiver->isCrypto()) {
    return this->cryptoMethodChecker->checkCryptoMethod(call, member);
  }
  if (receiver->isWeakRef()) {
    return this->weakRefChecker->checkWeakRefMethod(call, member);
  }
  if (receiver->isBytes()) {
    return this->bytesChecker->checkBytesMethod(call, member, expected);
  }

  return nullptr;
}

Expr *CallChecker::checkCallableReceiverCall(CallExpr *call, MemberExpr *member,
                                             const Type *expected) {
  const Type *receiver = member->object->type;
  if (receiver->isFunction() && receiver->hasThis()) {
    return this->checkThisReceiverCall(call, member);
  }
  if (receiver->isListParam()) {
    return this->checkListParamMethod(call, member);
  }
  if (receiver->isPromise()) {
    return this->checkPromiseReceiverCall(call, member, expected);
  }
  if (receiver->isTimer()) {
    return this->checkTimerReceiverCall(call, member);
  }

  return nullptr;
}

Expr *CallChecker::checkThisReceiverCall(CallExpr *call, MemberExpr *member) {
  auto made = this->functionMethodChecker->checkThisCall(call, member);
  if (made.isErr()) {
    this->state.getDiagnostics().error(member->loc(), made.error().message);
    return call;
  }

  return made.value();
}

Expr *CallChecker::checkTimerReceiverCall(CallExpr *call, MemberExpr *member) {
  auto ran = this->timerChecker->checkTimerMethod(call, member);
  if (ran.isOk()) {
    return ran.value();
  }
  if (ran.error().code != TimerCheckerCheckTimerMethodErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), ran.error().message);
  }

  return call;
}

Expr *CallChecker::checkListParamMethod(CallExpr *call, MemberExpr *member) {
  auto joined = this->functionMethodChecker->checkListMethod(call, member);
  if (joined.isOk()) {
    return joined.value();
  }
  if (joined.error().code != FunctionMethodCheckerCheckListMethodErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), joined.error().message);
  }

  return call;
}

Expr *CallChecker::checkPromiseReceiverCall(CallExpr *call, MemberExpr *member,
                                            const Type *expected) {
  auto chained = this->promiseChecker->checkPromiseMethod(call, member, expected);
  if (chained.isOk()) {
    return chained.value();
  }
  auto code = chained.error().code;
  if (code == PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED) {
    return call;
  }
  auto at = this->promiseMethodErrorPlace(call, member, code);
  this->state.getDiagnostics().error(at, chained.error().message);

  return call;
}

llvm::SMLoc
CallChecker::promiseMethodErrorPlace(const CallExpr *call, const MemberExpr *member,
                                     PromiseCheckerCheckPromiseMethodErrorCode code) const {
  switch (code) {
    case PromiseCheckerCheckPromiseMethodErrorCode::THEN_TWO_FUNCTIONS:
      return call->args[1]->loc();
    case PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED:
    case PromiseCheckerCheckPromiseMethodErrorCode::RESULT_METHOD:
    case PromiseCheckerCheckPromiseMethodErrorCode::NO_CATCH:
    case PromiseCheckerCheckPromiseMethodErrorCode::NO_FINALLY:
    case PromiseCheckerCheckPromiseMethodErrorCode::WRONG_ARITY:
    case PromiseCheckerCheckPromiseMethodErrorCode::NO_SUCH_METHOD:
      return member->loc();
  }

  return member->loc();
}

Expr *CallChecker::checkShapedReceiverCall(CallExpr *call, MemberExpr *member,
                                           const Type *expected) {
  this->walkMatchReceiver(member);
  const Type *receiver = member->object->type;
  if (!receiver->isUnion()) {
    return this->checkSettledReceiverCall(call, member, receiver, expected);
  }
  const auto *common = this->unionChecker->commonBaseWith(receiver, member->name);
  if (common == nullptr) {
    return this->checkUnionReceiverCall(call, member);
  }
  member->object = this->typeRelations->convert(member->object, common);

  return this->checkSettledReceiverCall(call, member, common, expected);
}

void CallChecker::walkMatchReceiver(MemberExpr *member) {
  if (!member->object->type->isMatch()) {
    return;
  }
  auto *values = this->matchChecker->matchItems(member->object);
  if (values != nullptr) {
    member->object = values;
  }
}

Expr *CallChecker::checkSettledReceiverCall(CallExpr *call, MemberExpr *member,
                                            const Type *receiver, const Type *expected) {
  const Type *classReceiver = member->object->type;
  bool isSubtleCall = classReceiver->isClass() &&
                      this->webCryptoChecker->isSubtleClass(classReceiver) &&
                      !this->libraryCalls->isInLibrary(call->loc());
  auto *subtle =
      isSubtleCall ? this->subtleCallChecker->checkSubtleCall(call, member, expected) : nullptr;
  if (subtle != nullptr) {
    return subtle;
  }
  if (receiver->isClass()) {
    return this->checkClassReceiverCall(call, member, expected);
  }
  this->rejectTypeArgs(member);

  return this->checkValueReceiverCall(call, member, receiver, expected);
}

Expr *CallChecker::checkUnionReceiverCall(CallExpr *call, MemberExpr *member) {
  this->rejectTypeArgs(member);
  auto unionCall = this->unionChecker->checkUnionMethodCall(call, member);
  if (unionCall.isOk()) {
    return unionCall.value();
  }
  this->state.getDiagnostics().error(member->loc(), unionCall.error().message);

  return call;
}

Expr *CallChecker::checkClassReceiverCall(CallExpr *call, MemberExpr *member,
                                          const Type *expected) {
  bool isProven = this->urlChecker->proveUrlSetter(member, call);
  auto *checked = this->methodCallChecker->checkMethodCall(call, member, expected);
  this->httpChecker->walkHeadersCall(call, member);
  if (isProven && checked->type->isResult()) {
    auto *dropped = this->state.getAst().make<UnaryExpr>(call->loc(), UnaryOp::Void, checked);
    dropped->type = Type::undefinedTy();
    return dropped;
  }

  return checked;
}

Expr *CallChecker::checkValueReceiverCall(CallExpr *call, MemberExpr *member, const Type *receiver,
                                          const Type *expected) {
  if (!receiver->isTuple()) {
    return this->checkNonTupleReceiverCall(call, member, receiver, expected);
  }
  const auto *arrayMethod = ArrayMethodTable::find(member->name);
  auto *copy = this->tupleArrayCopy(member, arrayMethod);
  if (copy == nullptr) {
    return this->checkTupleReceiverCall(call, member);
  }
  member->object = copy;

  return this->checkNonTupleReceiverCall(call, member, copy->type, expected);
}

Expr *CallChecker::checkNonTupleReceiverCall(CallExpr *call, MemberExpr *member,
                                             const Type *receiver, const Type *expected) {
  if (receiver->isObject()) {
    return this->checkFieldReceiverCall(call, member);
  }
  const auto *arrayMethod = ArrayMethodTable::find(member->name);
  bool isArrayPush =
      receiver->isArray() && arrayMethod != nullptr && arrayMethod->method == ArrayMethod::PUSH;
  if (!isArrayPush && this->rejectSpread(call)) {
    return call;
  }
  auto *locale = this->checkLocaleMethod(call, member);
  if (locale != nullptr) {
    return locale;
  }

  return this->checkPlainValueMethod(call, member, receiver, expected);
}

Expr *CallChecker::checkFieldReceiverCall(CallExpr *call, MemberExpr *member) {
  auto fieldCall = this->objectChecker->checkFieldCall(call, member);
  if (fieldCall.isOk()) {
    return fieldCall.value();
  }
  bool isTakingThis = fieldCall.error().code == ObjectCheckerCheckFieldCallErrorCode::TAKES_THIS;
  llvm::SMLoc at = isTakingThis ? call->callee->loc() : member->loc();
  this->state.getDiagnostics().error(at, fieldCall.error().message);

  return call;
}

Expr *CallChecker::tupleArrayCopy(MemberExpr *member, const ArrayMethodInfo *arrayMethod) {
  bool isJoining = arrayMethod != nullptr && arrayMethod->method == ArrayMethod::JOIN;
  bool isChanging = arrayMethod != nullptr && (arrayMethod->form.is<ResizingArrayMethod>() ||
                                               arrayMethod->form.is<RewritingArrayMethod>() ||
                                               arrayMethod->form.is<SortingArrayMethod>());
  if (isChanging || isJoining) {
    return nullptr;
  }

  return this->tupleChecker->arrayCopy(member->object);
}

Expr *CallChecker::checkTupleReceiverCall(CallExpr *call, MemberExpr *member) {
  auto method = this->tupleChecker->checkTupleMethod(call, member);
  if (method.isOk()) {
    return method.value();
  }
  if (method.error().code != TupleCheckerCheckTupleMethodErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), method.error().message);
  }

  return call;
}

Expr *CallChecker::checkLocaleMethod(CallExpr *call, MemberExpr *member) {
  const Type *valueReceiver = member->object->type;
  const auto *stringMethod = StringMethodTable::find(member->name);
  bool isStringLocale = valueReceiver->isString() && stringMethod != nullptr &&
                        stringMethod->form.is<LocaleStringMethod>();
  if (isStringLocale) {
    return this->intlMethodChecker->checkStringLocale(call, member);
  }
  auto localeMember = ObjectMemberTable::find(member->name);
  bool isNumberLocale = (valueReceiver->isNumeric() || valueReceiver->isDecimal()) &&
                        localeMember == ObjectMember::TO_LOCALE_STRING;
  if (isNumberLocale) {
    return this->intlMethodChecker->checkNumberLocale(call, member);
  }
  if (valueReceiver->isDecimal()) {
    return this->decimalChecker->checkDecimalMethod(call, member);
  }

  return nullptr;
}

Expr *CallChecker::checkPlainValueMethod(CallExpr *call, MemberExpr *member, const Type *receiver,
                                         const Type *expected) {
  if (receiver->isIterator()) {
    return this->iteratorMethods->checkIteratorMethod(call, member);
  }
  if (receiver->isDict()) {
    return this->checkDictReceiverCall(call, member);
  }
  if (receiver->isString()) {
    return this->stringMethods->checkStringMethod(call, member);
  }
  if (receiver->isArray()) {
    return this->checkArrayReceiverCall(call, member, expected);
  }
  if (receiver->isNumeric()) {
    return this->checkNumberReceiverCall(call, member);
  }
  auto objectMember = ObjectMemberTable::find(member->name);
  bool isBoolToString =
      receiver->isBool() && objectMember == ObjectMember::TO_STRING && call->args.empty();
  if (isBoolToString) {
    return this->checkBoolToString(call, member);
  }

  return this->reportNoMethod(call, member, receiver);
}

Expr *CallChecker::checkArrayReceiverCall(CallExpr *call, MemberExpr *member,
                                          const Type *expected) {
  auto arrayCall = this->arrayMethods->checkArrayMethod(call, member, expected);
  if (arrayCall.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arrayCall.error().message);
    return call;
  }

  return arrayCall.value();
}

Expr *CallChecker::checkNumberReceiverCall(CallExpr *call, MemberExpr *member) {
  auto made = this->numberMethods->checkNumberMethod(call, member);
  if (made.isErr()) {
    this->state.getDiagnostics().error(member->loc(), made.error().message);
    return call;
  }

  return made.value();
}

Expr *CallChecker::checkBoolToString(CallExpr *call, MemberExpr *member) {
  auto *text = this->concatChecker->toStringOperand(member->object);
  call->args.push_back(text);
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();

  return call;
}

Expr *CallChecker::checkDictReceiverCall(CallExpr *call, MemberExpr *member) {
  auto method = this->dictMethods->checkDictMethod(call, member);
  if (method.isOk()) {
    return method.value();
  }
  auto code = method.error().code;
  if (code == DictMethodsCheckDictMethodErrorCode::ALREADY_REPORTED) {
    return call;
  }
  auto at = this->dictMethodErrorPlace(call, member, code);
  this->state.getDiagnostics().error(at, method.error().message);

  return call;
}

llvm::SMLoc CallChecker::dictMethodErrorPlace(const CallExpr *call, const MemberExpr *member,
                                              DictMethodsCheckDictMethodErrorCode code) const {
  switch (code) {
    case DictMethodsCheckDictMethodErrorCode::FOR_EACH_THIS:
      return call->args[1]->loc();
    case DictMethodsCheckDictMethodErrorCode::NOT_A_SET:
      return call->args[0]->loc();
    case DictMethodsCheckDictMethodErrorCode::ALREADY_REPORTED:
    case DictMethodsCheckDictMethodErrorCode::HAS_OWN_PROPERTY:
    case DictMethodsCheckDictMethodErrorCode::MAY_BE_UNDEFINED:
    case DictMethodsCheckDictMethodErrorCode::NO_RECORD_METHODS:
    case DictMethodsCheckDictMethodErrorCode::WRONG_ARITY:
    case DictMethodsCheckDictMethodErrorCode::NO_SUCH_PROPERTY:
      return member->loc();
  }

  return member->loc();
}

Expr *CallChecker::reportNoMethod(CallExpr *call, MemberExpr *member, const Type *receiver) {
  this->reportMissingMethod(member, receiver);
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

void CallChecker::reportMissingMethod(const MemberExpr *member, const Type *receiver) {
  if (receiver->isOptional()) {
    auto message = ErrorTextCallMethod::undefined(member->name);
    this->state.getDiagnostics().error(member->object->loc(), message);
    return;
  }
  auto functionMethod = FunctionMethodTable::find(member->name);
  bool isSettingThis = functionMethod.has_value();
  if (receiver->isFunction() && isSettingThis) {
    auto message = ErrorTextCallMethod::setsThis(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    return;
  }
  if (receiver->isError()) {
    return;
  }
  auto message = ErrorTextCallMethod::noProperty(member->name, receiver->name());
  this->state.getDiagnostics().error(member->loc(), message);
}

Expr *CallChecker::checkNamedCall(CallExpr *call, NameExpr *name, const Type *expected) {
  auto *libraryCall = this->libraryCalls->checkLibCall(call, name);
  if (libraryCall != nullptr) {
    return libraryCall;
  }
  auto *resultCall = this->resultChecker->checkResultCall(call, name, expected);
  if (resultCall != nullptr) {
    return resultCall;
  }
  auto *jsonCall = this->jsonStaticChecker->checkJsonGlobal(call, name);
  if (jsonCall != nullptr) {
    return jsonCall;
  }
  auto timer = this->checkTimerCall(call, name);
  if (timer.has_value()) {
    return *timer;
  }
  auto global = this->checkGlobalFunctionCall(call, name);
  if (global.has_value()) {
    return *global;
  }

  return this->checkScopedCall(call, name, expected);
}

std::optional<Expr *> CallChecker::checkTimerCall(CallExpr *call, NameExpr *name) {
  auto task = this->timerChecker->checkAsyncGlobal(call, name);
  if (task.isOk()) {
    return task.value();
  }
  switch (task.error().code) {
    case TimerCheckerCheckAsyncGlobalErrorCode::NOT_A_TIMER:
      return std::nullopt;
    case TimerCheckerCheckAsyncGlobalErrorCode::ALREADY_REPORTED:
      return call;
    case TimerCheckerCheckAsyncGlobalErrorCode::TIMEOUT_CALLBACK_ARGUMENTS:
      this->state.getDiagnostics().error(call->args[2]->loc(), task.error().message);
      return call;
    case TimerCheckerCheckAsyncGlobalErrorCode::IMMEDIATE_CALLBACK_ARGUMENTS:
      this->state.getDiagnostics().error(call->args[1]->loc(), task.error().message);
      return call;
    case TimerCheckerCheckAsyncGlobalErrorCode::WRONG_ARITY:
      this->state.getDiagnostics().error(name->loc(), task.error().message);
      return call;
  }

  return std::nullopt;
}

std::optional<Expr *> CallChecker::checkGlobalFunctionCall(CallExpr *call, NameExpr *name) {
  auto global = this->globalFunctionChecker->checkGlobalCall(call, name);
  if (global.isOk()) {
    return global.value();
  }
  switch (global.error().code) {
    case GlobalFunctionCheckerCheckGlobalCallErrorCode::NOT_A_GLOBAL:
      return std::nullopt;
    case GlobalFunctionCheckerCheckGlobalCallErrorCode::ALREADY_REPORTED:
      return call;
    case GlobalFunctionCheckerCheckGlobalCallErrorCode::CALL_FAILED:
      this->state.getDiagnostics().error(name->loc(), global.error().message);
      return call;
    case GlobalFunctionCheckerCheckGlobalCallErrorCode::ARGUMENT_FAILED:
      this->state.getDiagnostics().error(call->args[0]->loc(), global.error().message);
      return call;
  }

  return std::nullopt;
}

Expr *CallChecker::checkScopedCall(CallExpr *call, NameExpr *name, const Type *expected) {
  auto entry = this->topLevelChecker->lookupValue(name->name);
  FuncSymbol *function = entry.func();
  VarSymbol *variable = entry.var();
  if (function != nullptr && variable == nullptr) {
    return this->checkFunctionNameCall(call, name, function, expected);
  }
  auto *known = this->nestedFunctions->variableFunction(variable);
  if (known != nullptr && known->sym != nullptr) {
    return this->checkKnownFunctionCall(call, name, variable, known);
  }
  auto calledGlobal = GlobalNameTable::find(name->name);
  bool isArrayCall = calledGlobal == GlobalName::ARRAY && variable == nullptr &&
                     function == nullptr && entry.cls() == nullptr && !call->hasSpread();
  if (isArrayCall) {
    auto *made =
        this->state.getAst().make<NewExpr>(call->loc(), name->name, name->typeArgs, call->args);
    return this->newChecker->checkNew(made, expected);
  }
  if (variable == nullptr && function == nullptr) {
    return this->reportNotCallable(call, name, entry);
  }

  return nullptr;
}

Expr *CallChecker::checkKnownFunctionCall(CallExpr *call, NameExpr *name, VarSymbol *variable,
                                          const FunctionDecl *known) {
  name->binding = NamedVar{.var = variable};
  this->nestedFunctions->noteUse(variable, name->loc());
  name->type = this->scopes->varType(variable);

  return this->checkDirectCall(call, known->sym, name->loc());
}

Expr *CallChecker::checkFunctionNameCall(CallExpr *call, NameExpr *name, FuncSymbol *function,
                                         const Type *expected) {
  auto *eventsCall = this->eventTargetChecker->checkLibraryCall(call, name, function);
  if (eventsCall != nullptr) {
    return eventsCall;
  }
  auto *streamCall = this->streamPipeline->checkLibraryCall(call, name, function);
  if (streamCall != nullptr) {
    return streamCall;
  }
  auto *checked = this->checkDirectCall(call, function, name->loc(), expected);
  this->tzModulesChecker->noteHttpsCall(call, function->decl);

  return checked;
}

Expr *CallChecker::reportNotCallable(CallExpr *call, NameExpr *name, const ScopeEntry &entry) {
  this->reportNotFunction(name, entry);
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

void CallChecker::reportNotFunction(NameExpr *name, const ScopeEntry &entry) {
  std::string why = entry.cls() == nullptr ? ErrorTextUnsupported::why(name->name) : std::string();
  if (!why.empty()) {
    auto message = ErrorTextCallName::noSupport(name->name, why);
    this->state.getDiagnostics().error(name->loc(), message);
    return;
  }
  auto *checked = this->nameChecker->checkName(name);
  if (checked->type->isError()) {
    return;
  }
  auto message = ErrorTextCallName::notFunc(name->name);
  this->state.getDiagnostics().error(name->loc(), message);
}

Expr *CallChecker::checkCalleeValueCall(CallExpr *call) {
  auto *calleeName = dyn_cast<NameExpr>(call->callee);
  call->callee = this->exprChecker->checkExpr(call->callee);
  if (call->optional) {
    call->callee = this->exprChecker->chainLink(call->callee);
  }
  auto callable = this->checkCalleeCallable(call->callee->type, calleeName);
  if (callable.isOk()) {
    return this->checkTypedValueCall(call, callable.value());
  }
  if (callable.error().code != CallCheckerCalleeCallableErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(call->callee->loc(), callable.error().message);
  }
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

Expr *CallChecker::checkTypedValueCall(CallExpr *call, const Type *type) {
  auto valued = this->checkValueCall(call, type);
  if (valued.isOk()) {
    return valued.value();
  }
  this->state.getDiagnostics().error(call->callee->loc(), valued.error().message);

  return call;
}

CallChecker::ResultTypeArgsUsable
CallChecker::checkDirectTypeArgs(const FuncSymbol *function,
                                 llvm::ArrayRef<TypeRef> typeArgs) const {
  if (!typeArgs.empty()) {
    auto message = ErrorTextCallTypeArgs::notGeneric(function->name);

    return err(CallCheckerDirectTypeArgsError{
        .code = CallCheckerDirectTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(function);
}

Expr *CallChecker::checkDirectCall(CallExpr *call, FuncSymbol *function, llvm::SMLoc loc,
                                   const Type *expected) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  llvm::ArrayRef<TypeRef> typeArgs =
      name != nullptr ? llvm::ArrayRef<TypeRef>(name->typeArgs) : llvm::ArrayRef<TypeRef>();
  if (function->isGeneric()) {
    return this->typeArgInferrer->checkGenericCall(call, function, loc, expected, typeArgs);
  }
  auto usable = this->checkDirectTypeArgs(function, typeArgs);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(loc, usable.error().message);
  }
  call->target = FunctionCall{.func = function};
  call->type = function->getReturnType();

  return this->argumentChecker->checkArguments(call, function->params, function->minArgs,
                                               function->hasRest, loc);
}

CallChecker::ResultValueCallTyped CallChecker::checkValueCall(CallExpr *call, const Type *type) {
  call->type = type->ret();
  if (type->hasThis()) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextValueCall::takesThis();

    return err(CallCheckerCheckValueCallError{
        .code = CallCheckerCheckValueCallErrorCode::TAKES_THIS,
        .message = message,
    });
  }
  auto *checked = this->argumentChecker->checkArguments(call, type->params(), type->minArgs(),
                                                        type->hasRest(), call->callee->loc());

  return ok(checked);
}

CallChecker::ResultNoSpreadPassed CallChecker::checkSpreadFree(const CallExpr *call,
                                                               size_t index) const {
  if (call->spreads(index)) {
    auto message = ErrorTextSpread::notRest();

    return err(CallCheckerSpreadFreeError{
        .code = CallCheckerSpreadFreeErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok(call);
}

bool CallChecker::rejectSpread(CallExpr *call) {
  for (size_t index = 0; index < call->args.size(); ++index) {
    auto plain = this->checkSpreadFree(call, index);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(call->args[index]->loc(), plain.error().message);
      this->argumentChecker->walkArgsLoosely(call);
      call->type = Type::error();
      return true;
    }
  }

  return false;
}

bool CallChecker::isUnshadowedGlobal(const NameExpr *name) const {
  if (this->builtinModuleChecker->nodeName(name->name) != nullptr) {
    return false;
  }
  auto shadow = this->scopes->lookup(name->name);

  return shadow.var() == nullptr && shadow.func() == nullptr && shadow.cls() == nullptr;
}

bool CallChecker::isGlobalName(const Expr *expr, GlobalName global) const {
  bool isThroughGlobalThis = global == GlobalName::CRYPTO && this->isGlobalThisMember(expr, global);
  if (isThroughGlobalThis) {
    return true;
  }

  return this->isNamingGlobal(expr, global);
}

bool CallChecker::isNamingGlobal(const Expr *expr, GlobalName global) const {
  auto name = GlobalNameTable::getName(global);
  auto *named = dyn_cast<NameExpr>(expr);
  if (named == nullptr || named->name != name) {
    return false;
  }

  return this->isUnshadowedGlobal(named);
}

bool CallChecker::isGlobalThisMember(const Expr *expr, GlobalName global) const {
  auto name = GlobalNameTable::getName(global);
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr || member->optional || member->name != name) {
    return false;
  }

  return this->isNamingGlobal(member->object, GlobalName::GLOBAL_THIS);
}

bool CallChecker::isGlobalMember(const MemberExpr *member, GlobalMember which) const {
  auto owner = GlobalMemberTable::getOwner(which);
  auto found = GlobalMemberTable::find(owner, member->name);
  if (found != which) {
    return false;
  }

  return this->isGlobalName(member->object, owner);
}
}
