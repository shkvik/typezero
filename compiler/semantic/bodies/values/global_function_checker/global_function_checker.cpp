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

GlobalFunctionChecker::GlobalFunctionChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->dateChecker = &all.dateChecker;
  this->exprChecker = &all.exprChecker;
  this->mathChecker = &all.mathChecker;
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
  static const GlobalName names[] = {GlobalName::STRING,     GlobalName::NUMBER,
                                     GlobalName::PARSE_INT,  GlobalName::PARSE_FLOAT,
                                     GlobalName::BOOLEAN,    GlobalName::DECIMAL,
                                     GlobalName::BIG_INT,    GlobalName::ENCODE_URI_COMPONENT,
                                     GlobalName::ENCODE_URI, GlobalName::DECODE_URI_COMPONENT,
                                     GlobalName::DECODE_URI, GlobalName::ESCAPE,
                                     GlobalName::UNESCAPE};
  if (this->callChecker->isGlobalName(name, GlobalName::REG_EXP)) {
    Expr *regex = this->callChecker->rejectSpread(call)
                      ? call
                      : this->regexChecker->checkRegExpConstructor(name->loc(), call->args);
    return ok(regex);
  }
  if (this->callChecker->isGlobalName(name, GlobalName::DATE)) {
    Expr *date = this->dateChecker->checkDateFunction(call, name);
    return ok(date);
  }
  auto isGlobal = [&](GlobalName global) { return this->callChecker->isGlobalName(name, global); };
  if (!llvm::any_of(names, isGlobal)) {
    return err(GlobalFunctionCheckerCheckGlobalCallError{
        .code = GlobalFunctionCheckerCheckGlobalCallErrorCode::NOT_A_GLOBAL,
        .message = "",
    });
  }

  ResultFunctionCalled called = this->checkGlobalFunction(call, name->name, name->loc());
  if (called.isOk()) {
    return ok(called.value());
  }
  GlobalFunctionCheckerCheckGlobalCallErrorCode code =
      GlobalFunctionCheckerCheckGlobalCallErrorCode::CALL_FAILED;
  switch (called.error().code) {
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED:
      code = GlobalFunctionCheckerCheckGlobalCallErrorCode::ALREADY_REPORTED;
      break;
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_UNDEFINED:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_WRONG_TYPE:
      code = GlobalFunctionCheckerCheckGlobalCallErrorCode::ARGUMENT_FAILED;
      break;
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::UNESCAPE_OF_ENCODE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_ESCAPE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_UNESCAPE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::DECODE_OF_ESCAPE:
    case GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY:
      break;
  }

  return err(GlobalFunctionCheckerCheckGlobalCallError{
      .code = code,
      .message = called.error().message,
  });
}

GlobalFunctionChecker::ResultFunctionCalled
GlobalFunctionChecker::checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  std::optional<GlobalName> global = GlobalNameTable::find(fn);
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (global == GlobalName::DECIMAL) {
    Expr *made = this->decimalChecker->checkDecimalCall(call, loc);
    return ok(made);
  }
  auto *encoded = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
  bool isEncodedInner =
      encoded && this->callChecker->isGlobalName(encoded->callee, GlobalName::ENCODE_URI_COMPONENT);
  if (global == GlobalName::UNESCAPE && isEncodedInner) {
    this->argumentChecker->walkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::toBytes();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::UNESCAPE_OF_ENCODE,
        .message = message,
    });
  }
  if (global == GlobalName::ESCAPE) {
    this->argumentChecker->walkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::escape();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_ESCAPE,
        .message = message,
    });
  }
  if (global == GlobalName::UNESCAPE) {
    this->argumentChecker->walkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::unescape();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_UNESCAPE,
        .message = message,
    });
  }
  static const std::pair<GlobalName, Builtin> uris[] = {
      {GlobalName::ENCODE_URI_COMPONENT, Builtin::UriEncodeComponent},
      {GlobalName::ENCODE_URI, Builtin::UriEncode},
      {GlobalName::DECODE_URI_COMPONENT, Builtin::UriDecodeComponent},
      {GlobalName::DECODE_URI, Builtin::UriDecode}};
  for (const auto &[uri, builtin] : uris) {
    if (global != uri) {
      continue;
    }
    auto *escaped = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
    if (escaped && global == GlobalName::DECODE_URI_COMPONENT &&
        this->callChecker->isGlobalName(escaped->callee, GlobalName::ESCAPE)) {
      for (Expr *&arg : escaped->args) {
        arg = this->exprChecker->checkExpr(arg);
      }
      escaped->type = Type::error();
      std::string message = ErrorTextGlobalFunction::fromBytes();

      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::DECODE_OF_ESCAPE,
          .message = message,
      });
    }
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    Expr *arg = this->exprChecker->checkExpr(call->args[0], Type::string());
    if (global == GlobalName::ENCODE_URI_COMPONENT &&
        (arg->type->isNumeric() || arg->type->isBool())) {
      call->args[0] = this->concatChecker->toStringOperand(arg);
    } else {
      call->args[0] = this->typeRelations->coerce(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = builtin};
    call->type = builtin == Builtin::UriDecodeComponent || builtin == Builtin::UriDecode
                     ? Type::result(Type::string(), this->prelude->builtinType("UriError"))
                     : Type::string();
    return ok(call);
  }
  if (global == GlobalName::BIG_INT) {
    std::string quoted = ErrorTextBigInt::quoted(fn);
    std::string bigint = ErrorTextBigInt::none(quoted);
    this->state.getDiagnostics().error(loc, bigint);
    this->argumentChecker->walkArgsLoosely(call);
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (global == GlobalName::BOOLEAN) {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
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
    Flow whenTrue, whenFalse;
    Expr *truth = this->truthChecker->checkBooleanCall(call, whenTrue, whenFalse);
    this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
    return ok(truth);
  }
  if (global == GlobalName::STRING || global == GlobalName::NUMBER) {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    if (global == GlobalName::STRING) {
      for (Expr *&arg : call->args) {
        arg = this->concatChecker->toStringOperand(this->exprChecker->checkExpr(arg));
      }
      call->target = BuiltinCall{.builtin = Builtin::StringOf};
      call->type = Type::string();
      return ok(call);
    }
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
      const Type *type = arg->type;
      const Type *base = type->nonOptional();
      if (type->isError()) {
        return err(GlobalFunctionCheckerCheckGlobalFunctionError{
            .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
            .message = "",
        });
      }
      if (type->isDate()) {
        Expr *time = this->dateChecker->dateTime(arg);
        return ok(time);
      }
      if (type->isOptional() || type->isUndefined()) {
        std::string message = ErrorTextGlobalFunction::undefined();

        return err(GlobalFunctionCheckerCheckGlobalFunctionError{
            .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_UNDEFINED,
            .message = message,
        });
      }
      if (base->isNumeric()) {
        arg = this->typeRelations->convert(arg, Type::doubleTy());
      } else if (!base->isBool() && !base->isString() && !base->isDecimal()) {
        std::string message = ErrorTextGlobalFunction::wrongType(type->name());

        return err(GlobalFunctionCheckerCheckGlobalFunctionError{
            .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_WRONG_TYPE,
            .message = message,
        });
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::NumberOf};
    call->type = !call->args.empty() && call->args[0]->type->isString()
                     ? Type::result(Type::doubleTy(), this->prelude->builtinType("NumberError"))
                     : Type::doubleTy();
    return ok(call);
  }

  if (global == GlobalName::PARSE_INT || global == GlobalName::PARSE_FLOAT) {
    bool isInt = global == GlobalName::PARSE_INT;
    ArgumentChecker::ResultArityMatches arity =
        this->argumentChecker->checkArity(call, 1, isInt ? 2 : 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
    if (call->args.size() == 2) {
      call->args[1] = this->binaryChecker->toInt32(this->mathChecker->numericArg(call->args[1]));
    }
    call->target = BuiltinCall{.builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat};
    call->type = Type::result(isInt ? Type::intTy() : Type::doubleTy(),
                              this->prelude->builtinType("NumberError"));
    return ok(call);
  }

  return ok(call);
}

}
