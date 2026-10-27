#include "compiler/check/values/number_methods/number_methods.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/values/global_function_checker/global_function_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

NumberMethods::NumberMethods(Checker &checker) : checker_(checker) {}

Expr *NumberMethods::checkStaticCall(CallExpr *call, MemberExpr *member) {
  bool isString = checker_.callChecker().isGlobalName(member->object, "String");
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  if (isString && fn == "raw") {
    if (!call->tagged) {
      checker_.diags_.error(member->loc(),
                            "String.raw can only tag a template, e.g. String.raw`C:\\dir`");
      for (Expr *&arg : call->args) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
      return call;
    }
    auto *texts = cast<ArrayLitExpr>(call->args[0]);
    auto *tmpl = checker_.ctx_.make<TemplateExpr>(texts->loc());
    for (const ArrayLitExpr::Element &e : texts->elements) {
      tmpl->quasis.push_back(cast<StringLitExpr>(e.expr)->value);
    }
    tmpl->exprs.assign(call->args.begin() + 1, call->args.end());
    return checker_.literalChecker().checkTemplate(tmpl);
  }
  if (isString && (fn == "fromCharCode" || fn == "fromCodePoint")) {
    bool codeUnits = fn == "fromCharCode";
    for (Expr *&arg : call->args) {
      arg = checker_.mathChecker().numericArg(arg);
      if (!codeUnits && arg->type->isNumeric()) {
        checker_.indexProofs().requireBetween(arg, 0, 0x10FFFF, "a code point");
      }
      arg = codeUnits ? checker_.binaryChecker().toInt32(arg)
                      : checker_.typeRelations().convert(arg, Type::doubleTy());
    }
    call->builtin = codeUnits ? Builtin::FromCharCode : Builtin::FromCodePoint;
    call->type = Type::string();
    return call;
  }
  if (!isString && (fn == "parseInt" || fn == "parseFloat")) {
    return checker_.globalFunctionChecker().checkGlobalFunction(call, fn, member->loc());
  }
  static const std::pair<llvm::StringLiteral, Builtin> predicates[] = {
      {"isInteger", Builtin::IsInteger},
      {"isSafeInteger", Builtin::IsSafeInteger},
  };
  if (!isString) {
    for (const auto &[name, builtin] : predicates) {
      if (fn != name) {
        continue;
      }
      if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
        return call;
      }
      call->args[0] = checker_.mathChecker().numericArg(call->args[0]);
      call->builtin = builtin;
      call->type = Type::boolTy();
      return call;
    }
  }
  checker_.diags_.error(member->loc(), "property '" + fn + "' does not exist on '" +
                                           (isString ? "String" : "Number") + "'");
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

Expr *NumberMethods::checkConversion(CallExpr *call, MemberExpr *member) {
  static const std::pair<llvm::StringLiteral, llvm::StringLiteral> conversions[] = {
      {"toInt", "int"},         {"toInt8", "int8"},     {"toInt16", "int16"},
      {"toInt32", "int32"},     {"toUint8", "uint8"},   {"toUint16", "uint16"},
      {"toUint32", "uint32"},   {"toUint64", "uint64"}, {"toDouble", "double"},
      {"toDecimal", "decimal"},
  };
  const Type *to = nullptr;
  for (const auto &[method, type] : conversions) {
    if (member->name == method) {
      to = Type::fromName(type);
    }
  }
  if (!to) {
    return nullptr;
  }
  call->type = Type::error();
  if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
    return call;
  }
  const Type *from = member->object->type;
  std::string text = "x";
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    text = name->name.str();
  }
  if (from->isFloat() && to->isDecimal()) {
    checker_.diags_.error(member->loc(),
                          "a double is inexact already, so it becomes a decimal through "
                          "'decimal(" +
                              text + ")', which says so");
    return call;
  }
  if (from->isFloat() && to->isInteger()) {
    auto *rounding = dyn_cast<CallExpr>(member->object);
    if (!rounding || !isRoundingBuiltin(rounding->builtin)) {
      checker_.diags_.error(member->loc(),
                            "a double becomes an integer only after rounding, which says what "
                            "happens to the fraction: 'Math.trunc(" +
                                text + ")." + member->name +
                                "()' (or Math.round, Math.floor, Math.ceil)");
      return call;
    }
  }
  call->builtin = Builtin::NumConvert;
  call->type = to;

  return call;
}

Expr *NumberMethods::checkNumberMethod(CallExpr *call, MemberExpr *member) {
  if (Expr *converted = checkConversion(call, member)) {
    return converted;
  }
  if (member->name == "toString" && call->args.size() == 1) {
    member->object = checker_.typeRelations().convert(member->object, Type::doubleTy());
    call->args[0] = checker_.memberChecker().checkPosition(call->args[0]);
    checker_.indexProofs().requireBetween(call->args[0], 2, 36, "the radix of 'toString'");
    call->builtin = Builtin::NumToRadix;
    call->type = Type::string();
    return call;
  }
  if (member->name == "toString") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    call->args.push_back(checker_.concatChecker().toStringOperand(member->object));
    call->builtin = Builtin::StringOf;
    call->type = Type::string();
    return call;
  }
  if (member->name == "toFixed") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    member->object = checker_.typeRelations().convert(member->object, Type::doubleTy());
    if (!call->args.empty()) {
      call->args[0] = checker_.memberChecker().checkPosition(call->args[0]);
      checker_.indexProofs().requireBetween(call->args[0], 0, 100,
                                            "the number of digits of 'toFixed'");
    }
    call->builtin = Builtin::NumToFixed;
    call->type = Type::string();
    return call;
  }
  checker_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                           member->object->type->name() + "'");
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }

  return call;
}

}
