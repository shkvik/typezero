#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/math/math_checker/math_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

namespace {

const GlobalName kGlobalFunctions[] = {
    GlobalName::STRING,     GlobalName::NUMBER,
    GlobalName::PARSE_INT,  GlobalName::PARSE_FLOAT,
    GlobalName::BOOLEAN,    GlobalName::DECIMAL,
    GlobalName::BIG_INT,    GlobalName::ENCODE_URI_COMPONENT,
    GlobalName::ENCODE_URI, GlobalName::DECODE_URI_COMPONENT,
    GlobalName::DECODE_URI, GlobalName::ESCAPE,
    GlobalName::UNESCAPE,
};

const std::pair<GlobalName, Builtin> kUriBuiltins[] = {
    {GlobalName::ENCODE_URI_COMPONENT, Builtin::UriEncodeComponent},
    {GlobalName::ENCODE_URI, Builtin::UriEncode},
    {GlobalName::DECODE_URI_COMPONENT, Builtin::UriDecodeComponent},
    {GlobalName::DECODE_URI, Builtin::UriDecode},
};

constexpr size_t kMaxParseIntArgs = 2;

}

GlobalFunctionChecker::GlobalFunctionChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->dateChecker = &all.dateChecker;
  this->exprChecker = &all.exprChecker;
  this->mathChecker = &all.mathChecker;
  this->flowChecker = &all.flowChecker;
  this->regexChecker = &all.regexChecker;
  this->truthChecker = &all.truthChecker;
  this->binaryChecker = &all.binaryChecker;
  this->concatChecker = &all.concatChecker;
  this->typeRelations = &all.typeRelations;
  this->decimalChecker = &all.decimalChecker;
  this->argumentChecker = &all.argumentChecker;
}

GlobalFunctionChecker::ResultGlobalCallMade GlobalFunctionChecker::checkGlobalCall(CallExpr *call,
                                                                                   NameExpr *name) {
  if (this->callChecker->isGlobalName(name, GlobalName::REG_EXP)) {
    auto *regex = this->regExpCall(call, name);

    return ok(regex);
  }
  if (this->callChecker->isGlobalName(name, GlobalName::DATE)) {
    auto *date = this->dateChecker->checkDateFunction(call, name);

    return ok(date);
  }
  if (!this->isGlobalFunction(name)) {
    return err(GlobalFunctionCheckerCheckGlobalCallError{
        .code = GlobalFunctionCheckerCheckGlobalCallErrorCode::NOT_A_GLOBAL,
        .message = "",
    });
  }

  auto called = this->checkGlobalFunction(call, name->name, name->loc());
  if (called.isOk()) {
    return ok(called.value());
  }
  auto code = this->globalCallCode(called.error().code);

  return err(GlobalFunctionCheckerCheckGlobalCallError{
      .code = code,
      .message = called.error().message,
  });
}

Expr *GlobalFunctionChecker::regExpCall(CallExpr *call, const NameExpr *name) {
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }

  return this->regexChecker->checkRegExpConstructor(name->loc(), call->args);
}

bool GlobalFunctionChecker::isGlobalFunction(const NameExpr *name) const {
  for (GlobalName global : kGlobalFunctions) {
    if (this->callChecker->isGlobalName(name, global)) {
      return true;
    }
  }

  return false;
}

GlobalFunctionCheckerCheckGlobalCallErrorCode GlobalFunctionChecker::globalCallCode(
    GlobalFunctionCheckerCheckGlobalFunctionErrorCode code) const {
  switch (code) {
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED:
      return GlobalFunctionCheckerCheckGlobalCallErrorCode::ALREADY_REPORTED;
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_UNDEFINED:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_WRONG_TYPE:
      return GlobalFunctionCheckerCheckGlobalCallErrorCode::ARGUMENT_FAILED;
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::UNESCAPE_OF_ENCODE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_ESCAPE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_UNESCAPE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::DECODE_OF_ESCAPE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY:
      return GlobalFunctionCheckerCheckGlobalCallErrorCode::CALL_FAILED;
  }

  return GlobalFunctionCheckerCheckGlobalCallErrorCode::CALL_FAILED;
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::checkGlobalFunction(CallExpr *call, llvm::StringRef functionName,
                                           llvm::SMLoc loc) {
  auto global = GlobalNameTable::find(functionName);
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (global == GlobalName::DECIMAL) {
    auto *made = this->decimalChecker->checkDecimalCall(call, loc);

    return ok(made);
  }
  auto modern = this->checkNotLegacyEscape(call, global);
  if (modern.isErr()) {
    return err(modern.error());
  }
  auto uri = this->uriBuiltin(global);
  if (uri.has_value()) {
    return this->checkUriCall(call, global, *uri);
  }
  if (global == GlobalName::BIG_INT) {
    return this->reportBigInt(call, functionName, loc);
  }
  if (global == GlobalName::BOOLEAN) {
    return this->checkBooleanCall(call);
  }
  if (global == GlobalName::STRING) {
    return this->checkStringCall(call);
  }
  if (global == GlobalName::NUMBER) {
    return this->checkNumberCall(call);
  }
  if (global == GlobalName::PARSE_INT || global == GlobalName::PARSE_FLOAT) {
    return this->checkParseCall(call, global);
  }

  return ok(call);
}

CallExpr *GlobalFunctionChecker::soleCallArg(const CallExpr *call) const {
  if (call->args.size() != 1) {
    return nullptr;
  }

  return dyn_cast<CallExpr>(call->args[0]);
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::checkNotLegacyEscape(CallExpr *call, std::optional<GlobalName> global) {
  const auto *encoded = this->soleCallArg(call);
  bool isEncodedInner =
      encoded != nullptr &&
      this->callChecker->isGlobalName(encoded->callee, GlobalName::ENCODE_URI_COMPONENT);
  if (global == GlobalName::UNESCAPE && isEncodedInner) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextGlobalFunction::toBytes();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::UNESCAPE_OF_ENCODE,
        .message = message,
    });
  }
  if (global == GlobalName::ESCAPE) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextGlobalFunction::escape();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_ESCAPE,
        .message = message,
    });
  }
  if (global == GlobalName::UNESCAPE) {
    this->argumentChecker->walkArgsLoosely(call);
    auto message = ErrorTextGlobalFunction::unescape();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_UNESCAPE,
        .message = message,
    });
  }

  return ok(call);
}

std::optional<Builtin> GlobalFunctionChecker::uriBuiltin(std::optional<GlobalName> global) const {
  for (const auto &[uri, builtin] : kUriBuiltins) {
    if (global == uri) {
      return builtin;
    }
  }

  return std::nullopt;
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::checkUriCall(CallExpr *call, std::optional<GlobalName> global,
                                    Builtin builtin) {
  auto *escaped = this->soleCallArg(call);
  bool isDecodingEscape = escaped != nullptr && global == GlobalName::DECODE_URI_COMPONENT &&
                          this->callChecker->isGlobalName(escaped->callee, GlobalName::ESCAPE);
  if (isDecodingEscape) {
    return this->rejectDecodedEscape(escaped);
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  auto *arg = this->exprChecker->checkExpr(call->args[0], Type::string());
  bool isPrimitive = arg->type->isNumeric() || arg->type->isBool();
  if (global == GlobalName::ENCODE_URI_COMPONENT && isPrimitive) {
    call->args[0] = this->concatChecker->toStringOperand(arg);
  } else {
    call->args[0] = this->typeRelations->coerce(arg, Type::string());
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = this->uriResultType(builtin);

  return ok(call);
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::rejectDecodedEscape(CallExpr *escaped) {
  for (Expr *&arg : escaped->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  escaped->type = Type::error();
  auto message = ErrorTextGlobalFunction::fromBytes();

  return err(GlobalFunctionCheckerCheckGlobalFunctionError{
      .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::DECODE_OF_ESCAPE,
      .message = message,
  });
}

const Type *GlobalFunctionChecker::uriResultType(Builtin builtin) {
  bool isDecoding = builtin == Builtin::UriDecodeComponent || builtin == Builtin::UriDecode;
  if (!isDecoding) {
    return Type::string();
  }
  const auto *failure = this->prelude->builtinType(PreludeType::URI_ERROR);

  return Type::result(Type::string(), failure);
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::reportBigInt(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc) {
  auto quoted = ErrorTextBigInt::quoted(functionName);
  auto bigint = ErrorTextBigInt::none(quoted);
  this->state.getDiagnostics().error(loc, bigint);
  this->argumentChecker->walkArgsLoosely(call);

  return err(GlobalFunctionCheckerCheckGlobalFunctionError{
      .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
      .message = "",
  });
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::checkBooleanCall(CallExpr *call) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (call->args.empty()) {
    auto *no = this->state.getAst().make<BoolLitExpr>(call->loc(), false);
    no->type = Type::boolTy();

    return ok(no);
  }
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  auto *truth = this->truthChecker->checkBooleanCall(call, whenTrue, whenFalse);
  auto joined = this->flowChecker->join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(joined);

  return ok(truth);
}

GlobalFunctionChecker::ResultFunctionCalled GlobalFunctionChecker::checkStringCall(CallExpr *call) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  for (Expr *&arg : call->args) {
    auto *checked = this->exprChecker->checkExpr(arg);
    arg = this->concatChecker->toStringOperand(checked);
  }
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();

  return ok(call);
}

GlobalFunctionChecker::ResultFunctionCalled GlobalFunctionChecker::checkNumberCall(CallExpr *call) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  if (call->args.empty()) {
    return this->numberOf(call);
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (type->isError()) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (type->isDate()) {
    auto *time = this->dateChecker->dateTime(arg);

    return ok(time);
  }
  auto numbered = this->checkNumberSource(type);
  if (numbered.isErr()) {
    return err(numbered.error());
  }
  if (type->nonOptional()->isNumeric()) {
    call->args[0] = this->typeRelations->convert(arg, Type::doubleTy());
  }

  return this->numberOf(call);
}

GlobalFunctionChecker::ResultNumberSource
GlobalFunctionChecker::checkNumberSource(const Type *type) const {
  const Type *base = type->nonOptional();
  if (type->isOptional() || type->isUndefined()) {
    auto message = ErrorTextGlobalFunction::undefined();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_UNDEFINED,
        .message = message,
    });
  }
  bool isConvertible = base->isNumeric() || base->isBool() || base->isString() || base->isDecimal();
  if (!isConvertible) {
    auto message = ErrorTextGlobalFunction::wrongType(type->name());

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_WRONG_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

GlobalFunctionChecker::ResultFunctionCalled GlobalFunctionChecker::numberOf(CallExpr *call) {
  call->target = BuiltinCall{.builtin = Builtin::NumberOf};
  bool isParsing = !call->args.empty() && call->args[0]->type->isString();
  if (!isParsing) {
    call->type = Type::doubleTy();

    return ok(call);
  }
  const auto *failure = this->prelude->builtinType(PreludeType::NUMBER_ERROR);
  call->type = Type::result(Type::doubleTy(), failure);

  return ok(call);
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::checkParseCall(CallExpr *call, std::optional<GlobalName> global) {
  bool isInt = global == GlobalName::PARSE_INT;
  size_t maxArgs = isInt ? kMaxParseIntArgs : 1;
  auto arity = this->argumentChecker->checkArity(call, 1, maxArgs);
  if (arity.isErr()) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  if (call->args.size() == kMaxParseIntArgs) {
    auto *radix = this->mathChecker->numericArg(call->args[1]);
    call->args[1] = this->binaryChecker->toInt32(radix);
  }
  call->target = BuiltinCall{.builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat};
  const auto *parsed = isInt ? Type::intTy() : Type::doubleTy();
  const auto *failure = this->prelude->builtinType(PreludeType::NUMBER_ERROR);
  call->type = Type::result(parsed, failure);

  return ok(call);
}

}
