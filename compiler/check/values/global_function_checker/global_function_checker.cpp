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

namespace {}

GlobalFunctionChecker::GlobalFunctionChecker(Checker &owner)
    : checker(owner) {}

Expr *GlobalFunctionChecker::checkGlobalCall(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"String",    "Number",
                                              "parseInt",  "parseFloat",
                                              "Boolean",   "decimal",
                                              "BigInt",    "encodeURIComponent",
                                              "encodeURI", "decodeURIComponent",
                                              "decodeURI", "escape",
                                              "unescape"};
  if (this->checker.callChecker().isGlobalName(name, "RegExp")) {
    return this->checker.callChecker().rejectSpread(call)
               ? call
               : this->checker.regexChecker().checkRegExpConstructor(name->loc(), call->args);
  }
  if (this->checker.callChecker().isGlobalName(name, "Date")) {
    return this->checker.dateChecker().checkDateFunction(call, name);
  }
  if (!llvm::any_of(names, [&](llvm::StringRef n) {
        return this->checker.callChecker().isGlobalName(name, n);
      })) {
    return nullptr;
  }

  return this->checkGlobalFunction(call, name->name, name->loc());
}

Expr *GlobalFunctionChecker::checkGlobalFunction(CallExpr *call, llvm::StringRef fn,
                                                 llvm::SMLoc loc) {
  call->type = Type::error();
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  if (fn == "decimal") {
    return this->checker.decimalChecker().checkDecimalCall(call, loc);
  }
  if (fn == "escape" || fn == "unescape") {
    return this->reportLegacyEscape(call, fn, loc);
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
    if (auto *inner = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
        inner && fn == "decodeURIComponent" &&
        this->checker.callChecker().isGlobalName(inner->callee, "escape")) {
      this->checker.diags_.error(
          loc, "'decodeURIComponent(escape(s))' reads the UTF-8 bytes that s holds as "
               "characters, a legacy trick; keep the bytes in a Uint8Array and decode "
               "them with 'new TextDecoder().decode(bytes)'");
      for (Expr *&arg : inner->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      inner->type = Type::error();
      return call;
    }
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
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
    return call;
  }
  if (fn == "BigInt") {
    this->checker.decimalChecker().reportBigInt(loc, "'BigInt'");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (fn == "Boolean") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    if (call->args.empty()) {
      auto *no = this->checker.ctx_.make<BoolLitExpr>(call->loc(), false);
      no->type = Type::boolTy();
      return no;
    }
    Checker::Flow whenTrue, whenFalse;
    Expr *truth = this->checker.truthChecker().checkBooleanCall(call, whenTrue, whenFalse);
    this->checker.flow_ = FlowChecker::join(whenTrue, whenFalse);
    return truth;
  }
  if (fn == "String" || fn == "Number") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    if (fn == "String") {
      for (Expr *&arg : call->args) {
        arg = this->checker.concatChecker().toStringOperand(
            this->checker.exprChecker().checkExpr(arg));
      }
      call->target = BuiltinCall{.builtin = Builtin::StringOf};
      call->type = Type::string();
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
      const Type *type = arg->type;
      const Type *base = type->nonOptional();
      if (type->isError()) {
        return call;
      }
      if (type->isDate()) {
        return this->checker.dateChecker().dateTime(arg);
      }
      if (type->isOptional() || type->isUndefined()) {
        this->checker.diags_.error(arg->loc(),
                                   "Number() of a value that may be undefined would be NaN, "
                                   "and tz has no NaN; check it first or give a default "
                                   "with '??'");
        return call;
      }
      if (base->isNumeric()) {
        arg = this->checker.typeRelations().convert(arg, Type::doubleTy());
      } else if (!base->isBool() && !base->isString() && !base->isDecimal()) {
        this->checker.diags_.error(arg->loc(),
                                   "Number() takes a number, a boolean, a string or a decimal, "
                                   "found '" +
                                       type->name() + "'");
        return call;
      }
    }
    call->target = BuiltinCall{.builtin = Builtin::NumberOf};
    call->type =
        !call->args.empty() && call->args[0]->type->isString()
            ? Type::result(Type::doubleTy(), this->checker.prelude().builtinType("NumberError"))
            : Type::doubleTy();
    return call;
  }

  if (fn == "parseInt" || fn == "parseFloat") {
    bool isInt = fn == "parseInt";
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, isInt ? 2 : 1);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    call->args[0] = this->checker.argumentChecker().checkArg(call->args[0], Type::string());
    if (call->args.size() == 2) {
      call->args[1] = this->checker.binaryChecker().toInt32(
          this->checker.mathChecker().numericArg(call->args[1]));
    }
    call->target = BuiltinCall{.builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat};
    call->type = Type::result(isInt ? Type::intTy() : Type::doubleTy(),
                              this->checker.prelude().builtinType("NumberError"));
    return call;
  }

  return call;
}

Expr *GlobalFunctionChecker::reportLegacyEscape(CallExpr *call, llvm::StringRef fn,
                                                llvm::SMLoc loc) {
  auto *inner = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
  if (fn == "unescape" && inner &&
      this->checker.callChecker().isGlobalName(inner->callee, "encodeURIComponent")) {
    this->checker.diags_.error(
        loc, "'unescape(encodeURIComponent(s))' makes a string of the UTF-8 bytes of s, "
             "a legacy trick; get the bytes themselves with 'new "
             "TextEncoder().encode(s)'");
  } else if (fn == "escape") {
    this->checker.diags_.error(
        loc, "'escape' is a legacy function whose '%uXXXX' escapes nothing decodes but "
             "'unescape'; encode with 'encodeURIComponent(s)'");
  } else {
    this->checker.diags_.error(
        loc, "'unescape' is a legacy function that decodes the '%uXXXX' escapes of "
             "'escape'; decode with 'decodeURIComponent(s)'");
  }
  for (Expr *&arg : call->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
  }

  return call;
}

}
