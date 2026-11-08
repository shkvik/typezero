#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker.h"
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
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

GlobalFunctionChecker::GlobalFunctionChecker(SemanticState &owner)
    : state(owner) {}

GlobalFunctionChecker::ResultGlobalCallMade GlobalFunctionChecker::checkGlobalCall(CallExpr *call,
                                                                                   NameExpr *name) {
  static const llvm::StringLiteral names[] = {"String",    "Number",
                                              "parseInt",  "parseFloat",
                                              "Boolean",   "decimal",
                                              "BigInt",    "encodeURIComponent",
                                              "encodeURI", "decodeURIComponent",
                                              "decodeURI", "escape",
                                              "unescape"};
  if (CallChecker(this->state).isGlobalName(name, "RegExp")) {
    Expr *regex = CallChecker(this->state).rejectSpread(call)
                      ? call
                      : RegexChecker(this->state).checkRegExpConstructor(name->loc(), call->args);
    return ok(regex);
  }
  if (CallChecker(this->state).isGlobalName(name, "Date")) {
    Expr *date = DateChecker(this->state).checkDateFunction(call, name);
    return ok(date);
  }
  auto isGlobal = [&](llvm::StringRef n) { return CallChecker(this->state).isGlobalName(name, n); };
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
  if (CallChecker(this->state).rejectSpread(call)) {
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (fn == "decimal") {
    Expr *made = DecimalChecker(this->state).checkDecimalCall(call, loc);
    return ok(made);
  }
  auto *encoded = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
  bool encodedInner =
      encoded && CallChecker(this->state).isGlobalName(encoded->callee, "encodeURIComponent");
  if (fn == "unescape" && encodedInner) {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::toBytes();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::UNESCAPE_OF_ENCODE,
        .message = message,
    });
  }
  if (fn == "escape") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    std::string message = ErrorTextGlobalFunction::escape();

    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::LEGACY_ESCAPE,
        .message = message,
    });
  }
  if (fn == "unescape") {
    ArgumentChecker(this->state).checkArgsLoosely(call);
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
        CallChecker(this->state).isGlobalName(escaped->callee, "escape")) {
      for (Expr *&arg : escaped->args) {
        arg = ExprChecker(this->state).checkExpr(arg);
      }
      escaped->type = Type::error();
      std::string message = ErrorTextGlobalFunction::fromBytes();

      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::DECODE_OF_ESCAPE,
          .message = message,
      });
    }
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    Expr *arg = ExprChecker(this->state).checkExpr(call->args[0], Type::string());
    if (fn == "encodeURIComponent" && (arg->type->isNumeric() || arg->type->isBool())) {
      call->args[0] = ConcatChecker(this->state).toStringOperand(arg);
    } else {
      call->args[0] = TypeRelations(this->state).coerce(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = builtin};
    call->type = builtin == Builtin::UriDecodeComponent || builtin == Builtin::UriDecode
                     ? Type::result(Type::string(), Prelude(this->state).builtinType("UriError"))
                     : Type::string();
    return ok(call);
  }
  if (fn == "BigInt") {
    std::string quoted = ErrorTextBigInt::quoted("BigInt");
    std::string bigint = ErrorTextBigInt::none(quoted);
    this->state.getDiagnostics().error(loc, bigint);
    ArgumentChecker(this->state).checkArgsLoosely(call);
    return err(GlobalFunctionCheckerCheckGlobalFunctionError{
        .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (fn == "Boolean") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
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
    Expr *truth = TruthChecker(this->state).checkBooleanCall(call, whenTrue, whenFalse);
    this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
    return ok(truth);
  }
  if (fn == "String" || fn == "Number") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    if (fn == "String") {
      for (Expr *&arg : call->args) {
        arg = ConcatChecker(this->state).toStringOperand(ExprChecker(this->state).checkExpr(arg));
      }
      call->target = BuiltinCall{.builtin = Builtin::StringOf};
      call->type = Type::string();
      return ok(call);
    }
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
      const Type *type = arg->type;
      const Type *base = type->nonOptional();
      if (type->isError()) {
        return err(GlobalFunctionCheckerCheckGlobalFunctionError{
            .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::ALREADY_REPORTED,
            .message = "",
        });
      }
      if (type->isDate()) {
        Expr *time = DateChecker(this->state).dateTime(arg);
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
        arg = TypeRelations(this->state).convert(arg, Type::doubleTy());
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
            ? Type::result(Type::doubleTy(), Prelude(this->state).builtinType("NumberError"))
            : Type::doubleTy();
    return ok(call);
  }

  if (fn == "parseInt" || fn == "parseFloat") {
    bool isInt = fn == "parseInt";
    ArgumentChecker::ResultArityMatches arity =
        ArgumentChecker(this->state).checkArity(call, 1, isInt ? 2 : 1);
    if (arity.isErr()) {
      return err(GlobalFunctionCheckerCheckGlobalFunctionError{
          .code = GlobalFunctionCheckerCheckGlobalFunctionErrorCode::WRONG_ARITY,
          .message = arity.error().message,
      });
    }
    call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::string());
    if (call->args.size() == 2) {
      call->args[1] =
          BinaryChecker(this->state).toInt32(MathChecker(this->state).numericArg(call->args[1]));
    }
    call->target = BuiltinCall{.builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat};
    call->type = Type::result(isInt ? Type::intTy() : Type::doubleTy(),
                              Prelude(this->state).builtinType("NumberError"));
    return ok(call);
  }

  return ok(call);
}

}
