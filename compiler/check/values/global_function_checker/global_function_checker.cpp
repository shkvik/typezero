#include "compiler/check/values/global_function_checker/global_function_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

GlobalFunctionChecker::GlobalFunctionChecker(Checker &owner)
    : checker(owner) {}

GlobalFunctionChecker::ResultGlobalCallMade GlobalFunctionChecker::checkGlobalCall(CallExpr *call,
                                                                                   NameExpr *name) {
  static const llvm::StringLiteral names[] = {"String",    "Number",
                                              "parseInt",  "parseFloat",
                                              "Boolean",   "decimal",
                                              "BigInt",    "encodeURIComponent",
                                              "encodeURI", "decodeURIComponent",
                                              "decodeURI", "escape",
                                              "unescape"};
  if (this->checker.callChecker().isGlobalName(name, "RegExp")) {
    Expr *regex =
        this->checker.callChecker().rejectSpread(call)
            ? call
            : this->checker.regexChecker().checkRegExpConstructor(name->loc(), call->args);
    return ok(regex);
  }
  if (this->checker.callChecker().isGlobalName(name, "Date")) {
    Expr *date = this->checker.dateChecker().checkDateFunction(call, name);
    return ok(date);
  }
  auto isGlobal = [&](llvm::StringRef n) {
    return this->checker.callChecker().isGlobalName(name, n);
  };
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
  call->type = Type::error();
  if (this->checker.callChecker().rejectSpread(call)) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (fn == "decimal") {
    Expr *made = this->checker.decimalChecker().checkDecimalCall(call, loc);
    return ok(made);
  }
  auto *encoded = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
  bool encodedInner =
      encoded && this->checker.callChecker().isGlobalName(encoded->callee, "encodeURIComponent");
  if (fn == "unescape" && encodedInner) {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::toBytes();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::UNESCAPE_OF_ENCODE,
        .message = message,
    });
  }
  if (fn == "escape") {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::escape();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_ESCAPE,
        .message = message,
    });
  }
  if (fn == "unescape") {
    this->checker.argumentChecker().checkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::unescape();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_UNESCAPE,
        .message = message,
    });
  }
  static const std::pair<llvm::StringLiteral, Builtin> uris[] = {
      {"encodeURIComponent", Builtin::UriEncodeComponent},
      {"encodeURI", Builtin::UriEncode},
      {"decodeURIComponent", Builtin::UriDecodeComponent},
      {"decodeURI", Builtin::UriDecode}};
  for (const auto &[name, builtin] : uris) {
    if (fn != name) {
      continue;
    }
    auto *escaped = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
    if (escaped && fn == "decodeURIComponent" &&
        this->checker.callChecker().isGlobalName(escaped->callee, "escape")) {
      for (Expr *&arg : escaped->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      escaped->type = Type::error();
      std::string message = ErrorTextGlobalFunction::fromBytes();

      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::DECODE_OF_ESCAPE,
          .message = message,
      });
    }
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    Expr *arg = this->checker.exprChecker().checkExpr(call->args[0], Type::string());
    if (fn == "encodeURIComponent" && (arg->type->isNumeric() || arg->type->isBool())) {
      call->args[0] = this->checker.concatChecker().toStringOperand(arg);
    } else {
      call->args[0] = this->checker.typeRelations().coerce(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = builtin};
    call->type = builtin == Builtin::UriDecodeComponent || builtin == Builtin::UriDecode
                     ? Type::result(Type::string(), this->checker.prelude().builtinType("UriError"))
                     : Type::string();
    return ok(call);
  }
  if (fn == "BigInt") {
    this->checker.decimalChecker().reportBigInt(loc, "'BigInt'");
    this->checker.argumentChecker().checkArgsLoosely(call);
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (fn == "Boolean") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    if (call->args.empty()) {
      auto *no = this->checker.ctx_.make<BoolLitExpr>(call->loc(), false);
      no->type = Type::boolTy();
      return ok(no);
    }
    Checker::Flow whenTrue, whenFalse;
    Expr *truth = this->checker.truthChecker().checkBooleanCall(call, whenTrue, whenFalse);
    this->checker.flow_ = FlowChecker::join(whenTrue, whenFalse);
    return ok(truth);
  }
  if (fn == "String" || fn == "Number") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    if (fn == "String") {
      for (Expr *&arg : call->args) {
        arg = this->checker.concatChecker().toStringOperand(
            this->checker.exprChecker().checkExpr(arg));
      }
      call->target = BuiltinCall{.builtin = Builtin::StringOf};
      call->type = Type::string();
      return ok(call);
    }
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
      const Type *type = arg->type;
      const Type *base = type->nonOptional();
      if (type->isError()) {
        return err(GlobalFunctionCheckerCheckGlobalFunctionError{
            .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
            .message = "",
        });
      }
      if (type->isDate()) {
        Expr *time = this->checker.dateChecker().dateTime(arg);
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
        arg = this->checker.typeRelations().convert(arg, Type::doubleTy());
      } else if (!base->isBool() && !base->isString() && !base->isDecimal()) {
        std::string message = ErrorTextGlobalFunction::wrongType(type->name());

        return err(GlobalFunctionCheckerCheckGlobalFunctionError{
            .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::NUMBER_OF_WRONG_TYPE,
            .message = message,
        });
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::NumberOf};
    call->type =
        !call->args.empty() && call->args[0]->type->isString()
            ? Type::result(Type::doubleTy(), this->checker.prelude().builtinType("NumberError"))
            : Type::doubleTy();
    return ok(call);
  }

  if (fn == "parseInt" || fn == "parseFloat") {
    bool isInt = fn == "parseInt";
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, isInt ? 2 : 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    call->args[0] = this->checker.argumentChecker().checkArg(call->args[0], Type::string());
    if (call->args.size() == 2) {
      call->args[1] = this->checker.binaryChecker().toInt32(
          this->checker.mathChecker().numericArg(call->args[1]));
    }
    call->target = BuiltinCall{.builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat};
    call->type = Type::result(isInt ? Type::intTy() : Type::doubleTy(),
                              this->checker.prelude().builtinType("NumberError"));
    return ok(call);
  }

  return ok(call);
}

}
