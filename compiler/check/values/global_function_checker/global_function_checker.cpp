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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const Type *wholeNumberType(const Type *type, const Type *expected) {
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  bool narrows =
      wanted && wanted->isInteger() && (type->isFloat() || type->bitWidth() > wanted->bitWidth());

  return narrows ? wanted : type;
}

}

GlobalFunctionChecker::GlobalFunctionChecker(Checker &checker) : checker_(checker) {}

Expr *GlobalFunctionChecker::checkGlobalCall(CallExpr *call, NameExpr *name, const Type *expected) {
  static const llvm::StringLiteral names[] = {
      "String",    "Number",  "parseInt", "parseFloat",         "isNaN",     "isFinite",
      "Boolean",   "decimal", "BigInt",   "encodeURIComponent", "encodeURI", "decodeURIComponent",
      "decodeURI", "escape",  "unescape"};
  if (checker_.callChecker().isGlobalName(name, "RegExp")) {
    return checker_.callChecker().rejectSpread(call)
               ? call
               : checker_.regexChecker().checkRegExpConstructor(name->loc(), call->args);
  }
  if (checker_.callChecker().isGlobalName(name, "Date")) {
    return checker_.dateChecker().checkDateFunction(call, name);
  }
  if (!llvm::any_of(
          names, [&](llvm::StringRef n) { return checker_.callChecker().isGlobalName(name, n); })) {
    return nullptr;
  }

  return checkGlobalFunction(call, name->name, name->loc(), expected);
}

Expr *GlobalFunctionChecker::checkGlobalFunction(CallExpr *call, llvm::StringRef fn,
                                                 llvm::SMLoc loc, const Type *expected) {
  call->type = Type::error();
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (fn == "decimal") {
    return checker_.decimalChecker().checkDecimalCall(call, loc);
  }
  if (fn == "escape" || fn == "unescape") {
    return reportLegacyEscape(call, fn, loc);
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
        checker_.callChecker().isGlobalName(inner->callee, "escape")) {
      checker_.diags_.error(loc,
                            "'decodeURIComponent(escape(s))' reads the UTF-8 bytes that s holds as "
                            "characters, a legacy trick; keep the bytes in a Uint8Array and decode "
                            "them with 'new TextDecoder().decode(bytes)'");
      for (Expr *&arg : inner->args) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
      inner->type = Type::error();
      return call;
    }
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    Expr *arg = checker_.exprChecker().checkExpr(call->args[0], Type::string());
    if (fn == "encodeURIComponent" && (arg->type->isNumeric() || arg->type->isBool())) {
      call->args[0] = checker_.concatChecker().toStringOperand(arg);
    } else {
      call->args[0] = checker_.typeRelations().coerce(arg, Type::string());
    }
    call->builtin = builtin;
    call->type = builtin == Builtin::UriDecodeComponent || builtin == Builtin::UriDecode
                     ? Type::result(Type::string(), checker_.prelude().builtinType("UriError"))
                     : Type::string();
    return call;
  }
  if (fn == "BigInt") {
    checker_.decimalChecker().reportBigInt(loc, "'BigInt'");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (fn == "Boolean") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    if (call->args.empty()) {
      auto *no = checker_.ctx_.make<BoolLitExpr>(call->loc(), false);
      no->type = Type::boolTy();
      return no;
    }
    Checker::Flow whenTrue, whenFalse;
    Expr *truth = checker_.truthChecker().checkBooleanCall(call, whenTrue, whenFalse);
    checker_.flow_ = FlowChecker::join(whenTrue, whenFalse);
    return truth;
  }
  if (fn == "String" || fn == "Number") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    if (fn == "String") {
      for (Expr *&arg : call->args) {
        arg = checker_.concatChecker().toStringOperand(checker_.exprChecker().checkExpr(arg));
      }
      call->builtin = Builtin::StringOf;
      call->type = Type::string();
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
      const Type *type = arg->type, *base = type->nonOptional();
      if (type->isError()) {
        return call;
      }
      if (type->isDate()) {
        return checker_.dateChecker().dateTime(arg);
      }
      if (base->isNumeric()) {
        arg = checker_.typeRelations().convert(
            arg, type->isOptional() ? Type::optional(Type::doubleTy()) : Type::doubleTy());
      } else if (!base->isBool() && !base->isString() && !base->isDecimal() &&
                 !type->isUndefined()) {
        checker_.diags_.error(arg->loc(),
                              "Number() takes a number, a boolean, a string or a decimal, "
                              "found '" +
                                  type->name() + "'");
        return call;
      }
    }
    call->builtin = Builtin::NumberOf;
    call->type = Type::doubleTy();
    return call;
  }

  if (fn == "parseInt" || fn == "parseFloat") {
    bool isInt = fn == "parseInt";
    if (!checker_.argumentChecker().checkArity(call, loc, 1, isInt ? 2 : 1)) {
      return call;
    }
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
    if (call->args.size() == 2) {
      call->args[1] =
          checker_.binaryChecker().toInt32(checker_.mathChecker().numericArg(call->args[1]));
    }
    call->builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat;
    call->type = isInt ? wholeNumberType(Type::doubleTy(), expected) : Type::doubleTy();
    return call;
  }

  if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
    return call;
  }
  call->args[0] = checker_.mathChecker().numericArg(call->args[0]);
  call->builtin = fn == "isNaN" ? Builtin::IsNaN : Builtin::IsFinite;
  call->type = Type::boolTy();

  return call;
}

Expr *GlobalFunctionChecker::reportLegacyEscape(CallExpr *call, llvm::StringRef fn,
                                                llvm::SMLoc loc) {
  auto *inner = call->args.size() == 1 ? dyn_cast<CallExpr>(call->args[0]) : nullptr;
  if (fn == "unescape" && inner &&
      checker_.callChecker().isGlobalName(inner->callee, "encodeURIComponent")) {
    checker_.diags_.error(
        loc, "'unescape(encodeURIComponent(s))' makes a string of the UTF-8 bytes of s, "
             "a legacy trick; get the bytes themselves with 'new "
             "TextEncoder().encode(s)'");
  } else if (fn == "escape") {
    checker_.diags_.error(
        loc, "'escape' is a legacy function whose '%uXXXX' escapes nothing decodes but "
             "'unescape'; encode with 'encodeURIComponent(s)'");
  } else {
    checker_.diags_.error(loc,
                          "'unescape' is a legacy function that decodes the '%uXXXX' escapes of "
                          "'escape'; decode with 'decodeURIComponent(s)'");
  }
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

}
