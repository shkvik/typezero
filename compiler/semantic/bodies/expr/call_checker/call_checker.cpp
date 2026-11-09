#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
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
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
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

CallChecker::CallChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->newChecker = &all.getNewChecker();
  this->urlChecker = &all.getUrlChecker();
  this->dateChecker = &all.getDateChecker();
  this->dictMethods = &all.getDictMethods();
  this->exprChecker = &all.getExprChecker();
  this->httpChecker = &all.getHttpChecker();
  this->intlChecker = &all.getIntlChecker();
  this->jsonChecker = &all.getJsonChecker();
  this->mathChecker = &all.getMathChecker();
  this->nameChecker = &all.getNameChecker();
  this->arrayMethods = &all.getArrayMethods();
  this->bytesChecker = &all.getBytesChecker();
  this->libraryCalls = &all.getLibraryCalls();
  this->matchChecker = &all.getMatchChecker();
  this->regexChecker = &all.getRegexChecker();
  this->timerChecker = &all.getTimerChecker();
  this->tupleChecker = &all.getTupleChecker();
  this->unionChecker = &all.getUnionChecker();
  this->concatChecker = &all.getConcatChecker();
  this->cryptoChecker = &all.getCryptoChecker();
  this->numberMethods = &all.getNumberMethods();
  this->objectChecker = &all.getObjectChecker();
  this->resultChecker = &all.getResultChecker();
  this->staticChecker = &all.getStaticChecker();
  this->stringMethods = &all.getStringMethods();
  this->typeRelations = &all.getTypeRelations();
  this->argListChecker = &all.getArgListChecker();
  this->consoleChecker = &all.getConsoleChecker();
  this->decimalChecker = &all.getDecimalChecker();
  this->genericChecker = &all.getGenericChecker();
  this->inheritChecker = &all.getInheritChecker();
  this->processChecker = &all.getProcessChecker();
  this->promiseChecker = &all.getPromiseChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->streamPipeline = &all.getStreamPipeline();
  this->unknownChecker = &all.getUnknownChecker();
  this->weakRefChecker = &all.getWeakRefChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->iteratorMethods = &all.getIteratorMethods();
  this->nestedFunctions = &all.getNestedFunctions();
  this->topLevelChecker = &all.getTopLevelChecker();
  this->typeArgInferrer = &all.getTypeArgInferrer();
  this->tzModulesChecker = &all.getTzModulesChecker();
  this->webCryptoChecker = &all.getWebCryptoChecker();
  this->dateFormatChecker = &all.getDateFormatChecker();
  this->intlMethodChecker = &all.getIntlMethodChecker();
  this->jsonStaticChecker = &all.getJsonStaticChecker();
  this->methodCallChecker = &all.getMethodCallChecker();
  this->subtleCallChecker = &all.getSubtleCallChecker();
  this->eventTargetChecker = &all.getEventTargetChecker();
  this->reflectCallChecker = &all.getReflectCallChecker();
  this->bufferStaticChecker = &all.getBufferStaticChecker();
  this->cryptoMethodChecker = &all.getCryptoMethodChecker();
  this->resultMethodChecker = &all.getResultMethodChecker();
  this->builtinModuleChecker = &all.getBuiltinModuleChecker();
  this->promiseStaticChecker = &all.getPromiseStaticChecker();
  this->runtimeObjectChecker = &all.getRuntimeObjectChecker();
  this->functionMethodChecker = &all.getFunctionMethodChecker();
  this->globalFunctionChecker = &all.getGlobalFunctionChecker();
}

CallChecker::ResultTypeArgsAbsent CallChecker::noTypeArgs(const MemberExpr *member) {
  if (!member->typeArgs.empty()) {
    std::string message = ErrorTextCallTypeArgs::forGeneric();

    return err(CallCheckerNoTypeArgsError{
        .code = CallCheckerNoTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(member);
}

CallChecker::ResultCalleeCallable CallChecker::calleeCallable(const Type *type,
                                                              const NameExpr *calleeName) {
  bool optional = type->isOptional() && type->inner()->isFunction();
  if (optional) {
    std::string message = ErrorTextCallee::undefined();

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
  if (!type->isFunction() && calleeName) {
    std::string message = ErrorTextCallee::notFunc(calleeName->name, type->name());

    return err(CallCheckerCalleeCallableError{
        .code = CallCheckerCalleeCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (!type->isFunction()) {
    std::string message = ErrorTextCallee::uncallable(type->name());

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
  if (auto *index = dyn_cast<IndexExpr>(call->callee); index && !index->optional) {
    if (auto *key = dyn_cast<MemberExpr>(index->index);
        key && (key->name == "iterator" || key->name == "asyncIterator") &&
        this->isGlobalName(key->object, "Symbol")) {
      call->callee = this->state.getAst().make<MemberExpr>(
          index->loc(), index->object,
          key->name == "iterator" ? "[Symbol.iterator]" : "[Symbol.asyncIterator]");
    }
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && name->name == "super") {
    return this->inheritChecker->checkSuperCall(call);
  }
  Expr *nodeCall = this->builtinModuleChecker->checkNodeCall(call, expected);
  if (nodeCall != nullptr) {
    return nodeCall;
  }
  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !call->optional) {
    if (this->isGlobalName(member->object, "Promise")) {
      return this->promiseStaticChecker->checkPromiseStatic(call, member, expected);
    }
    auto rejectTypeArgs = [&] {
      ResultTypeArgsAbsent absent = CallChecker::noTypeArgs(member);
      if (absent.isErr()) {
        this->state.getDiagnostics().error(member->loc(), absent.error().message);
        member->typeArgs.clear();
      }
    };
    if (auto *object = dyn_cast<NameExpr>(member->object);
        !object || !this->scopes->lookup(object->name).cls()) {
      static const llvm::StringLiteral globals[] = {
          "String",  "Number",  "Object", "Iterator", "Array",  "Map",  "Symbol", "Result",
          "console", "process", "Math",   "Bit",      "RegExp", "Date", "Intl",   "decimal"};
      auto named = [&](llvm::StringRef g) { return this->isGlobalName(object, g); };
      if (object && llvm::any_of(globals, named)) {
        rejectTypeArgs();
      }
    }
    if (auto *object = dyn_cast<NameExpr>(member->object); object && object->name == "super") {
      return this->inheritChecker->checkSuperMethodCall(call, member);
    }
    Expr *resultStatic = this->resultChecker->checkResultStatic(call, member);
    if (resultStatic != nullptr) {
      return resultStatic;
    }
    if (this->isGlobalName(member->object, "JSON")) {
      return this->jsonStaticChecker->checkJsonStatic(call, member);
    }
    if (this->isGlobalName(member->object, "Array") && member->name == "isArray") {
      return this->jsonChecker->checkArrayIsArray(call, member);
    }
    if (this->isGlobalName(member->object, "Json") && member->name == "isJson") {
      return this->jsonChecker->checkJsonIsJson(call, member);
    }
    if (this->isGlobalName(member->object, "Reflect")) {
      return this->reflectCallChecker->checkReflectCall(call, member, expected);
    }
    Expr *consoleCall = this->consoleChecker->checkConsoleCall(call, member);
    if (consoleCall != nullptr) {
      return consoleCall;
    }
    Expr *processCall = this->processChecker->checkProcessCall(call, member);
    if (processCall != nullptr) {
      return processCall;
    }
    MemberExpr *nested = dyn_cast<MemberExpr>(member->object);
    bool bitCall = this->isGlobalName(member->object, "Bit") ||
                   (nested != nullptr && this->isGlobalName(nested->object, "Bit"));
    if (bitCall) {
      return this->mathChecker->checkBitCall(call, member);
    }
    if (this->isGlobalName(member->object, "Math")) {
      return this->mathChecker->checkMathCall(call, member, expected);
    }
    if (this->isGlobalName(member->object, "RegExp")) {
      return this->regexChecker->checkRegExpStatic(call, member);
    }
    if (this->isGlobalName(member->object, "Intl")) {
      return this->dateFormatChecker->checkIntlCall(call, member);
    }
    bool intlMember = nested != nullptr && this->isGlobalName(nested->object, "Intl");
    if (intlMember && nested->name == "Locale") {
      return this->intlChecker->checkLocaleStatic(call, member, expected);
    }
    if (intlMember) {
      return this->intlChecker->checkIntlStatic(call, member);
    }
    if (this->isGlobalName(member->object, "decimal")) {
      return this->decimalChecker->checkDecimalStatic(call, member);
    }
    if (this->cryptoChecker->isX509Class(member->object)) {
      return this->cryptoChecker->checkX509Parse(call, member);
    }
    if (this->isGlobalName(member->object, "crypto")) {
      return this->cryptoChecker->checkWebCryptoCall(call, member);
    }
    Expr *bytesStatic = this->bufferStaticChecker->checkBytesStatic(call, member);
    if (bytesStatic != nullptr) {
      return bytesStatic;
    }
    if (this->isGlobalName(member->object, "String") ||
        this->isGlobalName(member->object, "Number")) {
      if (this->rejectSpread(call)) {
        return call;
      }
      NumberMethods::ResultStaticCallMade made = this->numberMethods->checkStaticCall(call, member);
      bool reported = made.isErr() &&
                      made.error().code == NumberMethodsCheckStaticCallErrorCode::ALREADY_REPORTED;
      if (made.isErr() && !reported) {
        this->state.getDiagnostics().error(member->loc(), made.error().message);
      }
      return made.isOk() ? made.value() : call;
    }
    if (this->isGlobalName(member->object, "Object")) {
      ObjectChecker::ResultObjectStaticCall objectCall =
          this->objectChecker->checkObjectCall(call, member, expected);
      if (objectCall.isOk()) {
        return objectCall.value();
      }
      llvm::SMLoc at = call->args.empty() ? member->loc() : call->args[0]->loc();
      switch (objectCall.error().code) {
        case ObjectCheckerCheckObjectCallErrorCode::ALREADY_REPORTED:
          return call;
        case ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_ARITY:
        case ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_ARITY:
        case ObjectCheckerCheckObjectCallErrorCode::NOT_SUPPORTED:
        case ObjectCheckerCheckObjectCallErrorCode::WRONG_ARITY:
          at = member->loc();
          break;
        case ObjectCheckerCheckObjectCallErrorCode::GROUP_BY_KEY:
          at = call->args[1]->loc();
          break;
        case ObjectCheckerCheckObjectCallErrorCode::FROM_ENTRIES_PAIRS:
        case ObjectCheckerCheckObjectCallErrorCode::MAY_BE_UNDEFINED:
        case ObjectCheckerCheckObjectCallErrorCode::DICT_ALWAYS_EMPTY:
        case ObjectCheckerCheckObjectCallErrorCode::NOT_AN_OBJECT:
        case ObjectCheckerCheckObjectCallErrorCode::EMITTER_FIELDS:
        case ObjectCheckerCheckObjectCallErrorCode::METHOD_FIELDS:
        case ObjectCheckerCheckObjectCallErrorCode::NO_COMMON_TYPE:
        case ObjectCheckerCheckObjectCallErrorCode::NO_FIELDS:
        case ObjectCheckerCheckObjectCallErrorCode::NUMERIC_ENUM:
          break;
      }
      this->state.getDiagnostics().error(at, objectCall.error().message);
      return call;
    }
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? this->scopes->lookup(object->name).cls() : nullptr) {
      return this->staticChecker->checkClassStaticCall(call, member, cls, expected);
    }
    if (object && object->name == "this") {
      this->state.getClassContext().rememberThisMember(member->name);
    }
    if (this->isGlobalName(member->object, "Iterator")) {
      IteratorMethods::ResultIteratorFromCall from =
          this->iteratorMethods->checkIteratorFrom(call, member);
      if (from.isOk()) {
        return from.value();
      }
      llvm::SMLoc at = member->loc();
      switch (from.error().code) {
        case IteratorMethodsCheckIteratorFromErrorCode::ALREADY_REPORTED:
          return call;
        case IteratorMethodsCheckIteratorFromErrorCode::NOT_ITERABLE:
          at = call->args[0]->loc();
          break;
        case IteratorMethodsCheckIteratorFromErrorCode::NOT_SUPPORTED:
        case IteratorMethodsCheckIteratorFromErrorCode::WRONG_ARITY:
          break;
      }
      this->state.getDiagnostics().error(at, from.error().message);
      return call;
    }
    if (this->isGlobalName(member->object, "Array")) {
      return this->iteratorMethods->checkArrayFrom(call, member);
    }
    if (this->isGlobalName(member->object, "Map")) {
      DictMethods::ResultMapFunctionTyped mapped = this->dictMethods->checkMapStatic(call, member);
      if (mapped.isOk()) {
        return mapped.value();
      }
      llvm::SMLoc at = member->loc();
      switch (mapped.error().code) {
        case DictMethodsCheckMapStaticErrorCode::ALREADY_REPORTED:
          return call;
        case DictMethodsCheckMapStaticErrorCode::WRONG_GROUP_KEY:
          at = call->args[1]->loc();
          break;
        case DictMethodsCheckMapStaticErrorCode::WRONG_ARITY:
        case DictMethodsCheckMapStaticErrorCode::NOT_SUPPORTED:
          break;
      }
      this->state.getDiagnostics().error(at, mapped.error().message);
      return call;
    }
    static const llvm::StringLiteral consumers[] = {"toArray", "forEach", "reduce",
                                                    "some",    "every",   "find"};
    if (llvm::is_contained(consumers, member->name)) {
      this->iteratorMethods->markFused(member->object);
    }
    member->object = this->genericChecker->apparent(this->exprChecker->checkExpr(member->object));
    this->state.getClassContext().forgetThisMember();
    if (member->optional) {
      member->object = this->exprChecker->chainLink(member->object);
    }
    const Type *receiver = member->object->type;
    if (!receiver->isClass() && !receiver->isUnion()) {
      rejectTypeArgs();
    }
    if (receiver->isRuntimeObject()) {
      return this->runtimeObjectChecker->checkRuntimeMethod(call, member, expected);
    }
    if (receiver->isResult()) {
      Expr *resultMethod = this->resultMethodChecker->checkResultMethod(call, member, expected);
      if (resultMethod != nullptr) {
        return resultMethod;
      }
    }
    if (receiver->nonOptional()->isJson()) {
      return this->jsonChecker->reportJsonMethod(call, member);
    }
    if (receiver->isRegExp()) {
      return this->regexChecker->checkRegexMethod(call, member);
    }
    if (receiver->isDate()) {
      Expr *dateMethod = this->dateChecker->checkDateMethod(call, member);
      if (dateMethod != nullptr) {
        return dateMethod;
      }
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
      Expr *bytesMethod = this->bytesChecker->checkBytesMethod(call, member, expected);
      if (bytesMethod != nullptr) {
        return bytesMethod;
      }
    }
    UnknownChecker::ResultMethodChecked unknown =
        this->unknownChecker->checkUnknownMethod(call, member);
    if (unknown.isOk()) {
      return unknown.value();
    }
    switch (unknown.error().code) {
      case UnknownCheckerCheckUnknownMethodErrorCode::NOT_UNKNOWN:
        break;
      case UnknownCheckerCheckUnknownMethodErrorCode::NOT_NARROWED:
        this->state.getDiagnostics().error(member->object->loc(), unknown.error().message);
        return call;
      case UnknownCheckerCheckUnknownMethodErrorCode::RUNTIME_CLASS:
      case UnknownCheckerCheckUnknownMethodErrorCode::CLASS_VALUE:
        this->state.getDiagnostics().error(member->loc(), unknown.error().message);
        return call;
    }
    if (receiver->isFunction() && receiver->hasThis()) {
      FunctionMethodChecker::ResultThisCallMade made =
          this->functionMethodChecker->checkThisCall(call, member);
      if (made.isErr()) {
        this->state.getDiagnostics().error(member->loc(), made.error().message);
        return call;
      }
      return made.value();
    }
    if (receiver->isListParam()) {
      FunctionMethodChecker::ResultParamsJoined joined =
          this->functionMethodChecker->checkListMethod(call, member);
      bool reported =
          joined.isErr() &&
          joined.error().code == FunctionMethodCheckerCheckListMethodErrorCode::ALREADY_REPORTED;
      if (joined.isErr() && !reported) {
        this->state.getDiagnostics().error(member->loc(), joined.error().message);
      }
      return joined.isOk() ? joined.value() : call;
    }
    if (receiver->isPromise()) {
      PromiseChecker::ResultPromiseChained chained =
          this->promiseChecker->checkPromiseMethod(call, member, expected);
      if (chained.isOk()) {
        return chained.value();
      }
      llvm::SMLoc at = member->loc();
      switch (chained.error().code) {
        case PromiseCheckerCheckPromiseMethodErrorCode::ALREADY_REPORTED:
          return call;
        case PromiseCheckerCheckPromiseMethodErrorCode::THEN_TWO_FUNCTIONS:
          at = call->args[1]->loc();
          break;
        case PromiseCheckerCheckPromiseMethodErrorCode::RESULT_METHOD:
        case PromiseCheckerCheckPromiseMethodErrorCode::NO_CATCH:
        case PromiseCheckerCheckPromiseMethodErrorCode::NO_FINALLY:
        case PromiseCheckerCheckPromiseMethodErrorCode::WRONG_ARITY:
        case PromiseCheckerCheckPromiseMethodErrorCode::NO_SUCH_METHOD:
          break;
      }
      this->state.getDiagnostics().error(at, chained.error().message);
      return call;
    }
    if (receiver->isTimer()) {
      TimerChecker::ResultTimerMethodRun ran = this->timerChecker->checkTimerMethod(call, member);
      bool reported = ran.isErr() &&
                      ran.error().code == TimerCheckerCheckTimerMethodErrorCode::ALREADY_REPORTED;
      if (ran.isErr() && !reported) {
        this->state.getDiagnostics().error(member->loc(), ran.error().message);
      }
      return ran.isOk() ? ran.value() : call;
    }
    if (member->object->type->isMatch()) {
      Expr *values = this->matchChecker->matchItems(member->object);
      if (values != nullptr) {
        member->object = values;
        receiver = values->type;
      }
    }
    if (receiver->isUnion()) {
      const Type *common = this->unionChecker->commonBaseWith(receiver, member->name);
      if (!common) {
        rejectTypeArgs();
        UnionChecker::ResultUnionMethodTyped unionCall =
            this->unionChecker->checkUnionMethodCall(call, member);
        if (unionCall.isErr()) {
          this->state.getDiagnostics().error(member->loc(), unionCall.error().message);
        }
        return unionCall.isOk() ? unionCall.value() : call;
      }
      member->object = this->typeRelations->convert(member->object, common);
      receiver = common;
    }
    const Type *classReceiver = member->object->type;
    bool subtleCall = classReceiver->isClass() &&
                      this->webCryptoChecker->isSubtleClass(classReceiver, "SubtleCrypto") &&
                      !this->libraryCalls->inLibrary(call->loc());
    if (subtleCall) {
      Expr *subtle = this->subtleCallChecker->checkSubtleCall(call, member, expected);
      if (subtle != nullptr) {
        return subtle;
      }
    }
    if (receiver->isClass()) {
      bool proven = this->urlChecker->checkUrlSetter(member, call);
      Expr *checked = this->methodCallChecker->checkMethodCall(call, member, expected);
      this->httpChecker->checkHeadersCall(call, member);
      if (proven && checked->type->isResult()) {
        auto *dropped = this->state.getAst().make<UnaryExpr>(call->loc(), UnaryOp::Void, checked);
        dropped->type = Type::undefinedTy();
        return dropped;
      }
      return checked;
    }
    rejectTypeArgs();
    if (receiver->isTuple()) {
      static const char *const inPlace[] = {"push", "pop",     "shift", "unshift",    "splice",
                                            "sort", "reverse", "fill",  "copyWithin", "join"};
      Expr *copy = llvm::is_contained(inPlace, member->name)
                       ? nullptr
                       : this->tupleChecker->arrayCopy(member->object);
      if (!copy) {
        TupleChecker::ResultTupleMethod method = this->tupleChecker->checkTupleMethod(call, member);
        bool reported =
            method.isErr() &&
            method.error().code == TupleCheckerCheckTupleMethodErrorCode::ALREADY_REPORTED;
        if (method.isErr() && !reported) {
          this->state.getDiagnostics().error(member->loc(), method.error().message);
        }
        return method.isOk() ? method.value() : call;
      }
      member->object = copy;
      receiver = copy->type;
    }
    if (receiver->isObject()) {
      ObjectChecker::ResultFieldCallIsTyped fieldCall =
          this->objectChecker->checkFieldCall(call, member);
      if (fieldCall.isOk()) {
        return fieldCall.value();
      }
      bool takesThis = fieldCall.error().code == ObjectCheckerCheckFieldCallErrorCode::TAKES_THIS;
      llvm::SMLoc at = takesThis ? call->callee->loc() : member->loc();
      this->state.getDiagnostics().error(at, fieldCall.error().message);
      return call;
    }
    if (!(receiver->isArray() && member->name == "push") && this->rejectSpread(call)) {
      return call;
    }
    const Type *valueReceiver = member->object->type;
    bool stringLocale = valueReceiver->isString() &&
                        (member->name == "localeCompare" || member->name == "normalize");
    if (stringLocale) {
      return this->intlMethodChecker->checkStringLocale(call, member);
    }
    bool numberLocale = (valueReceiver->isNumeric() || valueReceiver->isDecimal()) &&
                        member->name == "toLocaleString";
    if (numberLocale) {
      return this->intlMethodChecker->checkNumberLocale(call, member);
    }
    if (valueReceiver->isDecimal()) {
      return this->decimalChecker->checkDecimalMethod(call, member);
    }
    if (receiver->isIterator()) {
      return this->iteratorMethods->checkIteratorMethod(call, member);
    }
    if (receiver->isDict()) {
      DictMethods::ResultDictMethodCalled method = this->dictMethods->checkDictMethod(call, member);
      if (method.isOk()) {
        return method.value();
      }
      llvm::SMLoc at = member->loc();
      switch (method.error().code) {
        case DictMethodsCheckDictMethodErrorCode::ALREADY_REPORTED:
          return call;
        case DictMethodsCheckDictMethodErrorCode::FOR_EACH_THIS:
          at = call->args[1]->loc();
          break;
        case DictMethodsCheckDictMethodErrorCode::NOT_A_SET:
          at = call->args[0]->loc();
          break;
        case DictMethodsCheckDictMethodErrorCode::HAS_OWN_PROPERTY:
        case DictMethodsCheckDictMethodErrorCode::MAY_BE_UNDEFINED:
        case DictMethodsCheckDictMethodErrorCode::NO_RECORD_METHODS:
        case DictMethodsCheckDictMethodErrorCode::WRONG_ARITY:
        case DictMethodsCheckDictMethodErrorCode::NO_SUCH_PROPERTY:
          break;
      }
      this->state.getDiagnostics().error(at, method.error().message);
      return call;
    }
    if (receiver->isString()) {
      return this->stringMethods->checkStringMethod(call, member);
    }
    if (receiver->isArray()) {
      ArrayMethods::ResultArrayMethodCall arrayCall =
          this->arrayMethods->checkArrayMethod(call, member, expected);
      if (arrayCall.isErr()) {
        this->state.getDiagnostics().error(member->loc(), arrayCall.error().message);
        return call;
      }
      return arrayCall.value();
    }
    if (receiver->isNumeric()) {
      NumberMethods::ResultMethodCallMade made =
          this->numberMethods->checkNumberMethod(call, member);
      if (made.isErr()) {
        this->state.getDiagnostics().error(member->loc(), made.error().message);
        return call;
      }
      return made.value();
    }
    if (receiver->isBool() && member->name == "toString" && call->args.empty()) {
      call->args.push_back(this->concatChecker->toStringOperand(member->object));
      call->target = BuiltinCall{.builtin = Builtin::StringOf};
      call->type = Type::string();
      return call;
    }
    bool setsThis = member->name == "call" || member->name == "apply" || member->name == "bind";
    if (receiver->isOptional()) {
      std::string message = ErrorTextCallMethod::undefined(member->name);
      this->state.getDiagnostics().error(member->object->loc(), message);
    } else if (receiver->isFunction() && setsThis) {
      std::string message = ErrorTextCallMethod::setsThis(member->name);
      this->state.getDiagnostics().error(member->loc(), message);
    } else if (!receiver->isError()) {
      std::string message = ErrorTextCallMethod::noProperty(member->name, receiver->name());
      this->state.getDiagnostics().error(member->loc(), message);
    }
    this->argumentChecker->checkArgsLoosely(call);
    return call;
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && !call->optional) {
    Expr *libraryCall = this->libraryCalls->checkLibCall(call, name);
    if (libraryCall != nullptr) {
      return libraryCall;
    }
    Expr *resultCall = this->resultChecker->checkResultCall(call, name, expected);
    if (resultCall != nullptr) {
      return resultCall;
    }
    Expr *jsonCall = this->jsonStaticChecker->checkJsonGlobal(call, name);
    if (jsonCall != nullptr) {
      return jsonCall;
    }
    TimerChecker::ResultTimerScheduled task = this->timerChecker->checkAsyncGlobal(call, name);
    if (task.isOk()) {
      return task.value();
    }
    switch (task.error().code) {
      case TimerCheckerCheckAsyncGlobalErrorCode::NOT_A_TIMER:
        break;
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
    GlobalFunctionChecker::ResultGlobalCallMade global =
        this->globalFunctionChecker->checkGlobalCall(call, name);
    if (global.isOk()) {
      return global.value();
    }
    switch (global.error().code) {
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::NOT_A_GLOBAL:
        break;
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::ALREADY_REPORTED:
        return call;
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::CALL_FAILED:
        this->state.getDiagnostics().error(name->loc(), global.error().message);
        return call;
      case GlobalFunctionCheckerCheckGlobalCallErrorCode::ARGUMENT_FAILED:
        this->state.getDiagnostics().error(call->args[0]->loc(), global.error().message);
        return call;
    }
    ScopeEntry entry = this->topLevelChecker->lookupValue(name->name);
    if (entry.func() && !entry.var()) {
      Expr *eventsCall = this->eventTargetChecker->checkLibraryCall(call, name, entry.func());
      if (eventsCall != nullptr) {
        return eventsCall;
      }
      Expr *streamCall = this->streamPipeline->checkLibraryCall(call, name, entry.func());
      if (streamCall != nullptr) {
        return streamCall;
      }
    }
    if (entry.func() && !entry.var()) {
      Expr *checked = this->checkDirectCall(call, entry.func(), name->loc(), expected);
      this->tzModulesChecker->noteHttpsCall(call, entry.func()->decl);
      return checked;
    }
    VarSymbol *var = entry.var();
    FunctionDecl *known = var ? (var->function() ? var->function()
                                 : var->isConst  ? var->lambda()
                                                 : nullptr)
                              : nullptr;
    if (known && known->sym) {
      name->binding = NamedVar{.var = var};
      this->nestedFunctions->noteUse(var, name->loc());
      name->type = this->scopes->varType(var);
      return this->checkDirectCall(call, known->sym, name->loc());
    }
    if (!entry.var() && !entry.func() && !entry.cls() && name->name == "Array" &&
        !call->hasSpread()) {
      auto *made =
          this->state.getAst().make<NewExpr>(call->loc(), name->name, name->typeArgs, call->args);

      return this->newChecker->checkNew(made, expected);
    }
    if (!entry.var() && !entry.func()) {
      std::string why = !entry.cls() ? ErrorTextUnsupported::why(name->name) : std::string();
      if (!why.empty()) {
        std::string message = ErrorTextCallName::noSupport(name->name, why);
        this->state.getDiagnostics().error(name->loc(), message);
      } else if (!this->nameChecker->checkName(name)->type->isError()) {
        std::string message = ErrorTextCallName::notFunc(name->name);
        this->state.getDiagnostics().error(name->loc(), message);
      }
      this->argumentChecker->checkArgsLoosely(call);
      return call;
    }
  }

  auto *calleeName = dyn_cast<NameExpr>(call->callee);
  call->callee = this->exprChecker->checkExpr(call->callee);
  if (call->optional) {
    call->callee = this->exprChecker->chainLink(call->callee);
  }
  ResultCalleeCallable callable = CallChecker::calleeCallable(call->callee->type, calleeName);
  if (callable.isOk()) {
    ResultValueCallTyped valued = this->checkValueCall(call, callable.value());
    if (valued.isErr()) {
      this->state.getDiagnostics().error(call->callee->loc(), valued.error().message);
    }
    return valued.isOk() ? valued.value() : call;
  }
  if (callable.error().code != CallCheckerCalleeCallableErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(call->callee->loc(), callable.error().message);
  }
  this->argumentChecker->checkArgsLoosely(call);

  return call;
}

CallChecker::ResultTypeArgsUsable CallChecker::directTypeArgs(const FuncSymbol *func,
                                                              llvm::ArrayRef<TypeRef> typeArgs) {
  if (!typeArgs.empty()) {
    std::string message = ErrorTextCallTypeArgs::notGeneric(func->name);

    return err(CallCheckerDirectTypeArgsError{
        .code = CallCheckerDirectTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(func);
}

Expr *CallChecker::checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc,
                                   const Type *expected) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  llvm::ArrayRef<TypeRef> typeArgs =
      name ? llvm::ArrayRef<TypeRef>(name->typeArgs) : llvm::ArrayRef<TypeRef>();
  if (func->isGeneric()) {
    return this->typeArgInferrer->checkGenericCall(call, func, loc, expected, typeArgs);
  }
  ResultTypeArgsUsable usable = CallChecker::directTypeArgs(func, typeArgs);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(loc, usable.error().message);
  }
  this->returnInferrer->resolveParams(func);
  call->target = FunctionCall{.func = func};
  call->type = func->getReturnType();

  return this->argumentChecker->checkArguments(call, func->params, func->minArgs, func->hasRest,
                                               loc);
}

CallChecker::ResultValueCallTyped CallChecker::checkValueCall(CallExpr *call, const Type *type) {
  call->type = type->ret();
  if (type->hasThis()) {
    this->argumentChecker->checkArgsLoosely(call);
    std::string message = ErrorTextValueCall::takesThis();

    return err(CallCheckerCheckValueCallError{
        .code = CallCheckerCheckValueCallErrorCode::TAKES_THIS,
        .message = message,
    });
  }
  Expr *checked = this->argumentChecker->checkArguments(call, type->params(), type->minArgs(),
                                                        type->hasRest(), call->callee->loc());

  return ok(checked);
}

CallChecker::ResultNoSpreadPassed CallChecker::spreadFree(const CallExpr *call, size_t i) {
  if (call->spreads(i)) {
    std::string message = ErrorTextSpread::notRest();

    return err(CallCheckerSpreadFreeError{
        .code = CallCheckerSpreadFreeErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok(call);
}

bool CallChecker::rejectSpread(CallExpr *call) {
  for (size_t i = 0; i < call->args.size(); ++i) {
    ResultNoSpreadPassed plain = CallChecker::spreadFree(call, i);
    if (plain.isOk()) {
      continue;
    }
    this->state.getDiagnostics().error(call->args[i]->loc(), plain.error().message);
    this->argumentChecker->checkArgsLoosely(call);
    call->type = Type::error();

    return true;
  }

  return false;
}

bool CallChecker::isGlobalName(const Expr *expr, llvm::StringRef name) const {
  if (auto *m = dyn_cast<MemberExpr>(expr); m && name == "crypto" && m->name == "crypto" &&
                                            !m->optional &&
                                            this->isGlobalName(m->object, "globalThis")) {
    return true;
  }
  auto *n = dyn_cast<NameExpr>(expr);
  if (!n || n->name != name) {
    return false;
  }
  if (this->builtinModuleChecker->nodeName(name)) {
    return false;
  }
  ScopeEntry shadow = this->scopes->lookup(name);

  return !shadow.var() && !shadow.func() && !shadow.cls();
}

}
