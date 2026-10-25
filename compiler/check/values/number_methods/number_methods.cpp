#include "compiler/check/values/number_methods/number_methods.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/check/values/global_function_checker/global_function_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

NumberMethods::NumberMethods(Sema &sema) : sema_(sema) {}

// String.fromCharCode, String.fromCodePoint, and the functions of Number.
Expr *NumberMethods::checkStaticCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  bool isString = sema_.callChecker().isGlobalName(member->object, "String");
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  if (isString && fn == "raw") {
    // String.raw`C:\dir` is the template itself, with its texts as written
    // (see Parser::parseCallOrMember).
    if (!call->tagged) {
      sema_.diags_.error(member->loc(), "String.raw can only tag a template, e.g. String.raw`C:\\dir`");
      for (Expr *&arg : call->args) {
        arg = sema_.exprChecker().checkExpr(arg);
      }
      return call;
    }
    auto *texts = cast<ArrayLitExpr>(call->args[0]);
    auto *tmpl = sema_.ctx_.make<TemplateExpr>(texts->loc());
    for (const ArrayLitExpr::Element &e : texts->elements) {
      tmpl->quasis.push_back(cast<StringLitExpr>(e.expr)->value);
    }
    tmpl->exprs.assign(call->args.begin() + 1, call->args.end());
    return sema_.literalChecker().checkTemplate(tmpl);
  }
  if (isString && (fn == "fromCharCode" || fn == "fromCodePoint")) {
    bool codeUnits = fn == "fromCharCode";
    for (Expr *&arg : call->args) {
      // Code units are taken modulo 2^16, as JavaScript does; a code point
      // must be one, or the program stops with a RangeError.
      arg = sema_.mathChecker().numericArg(arg);
      arg = codeUnits ? sema_.binaryChecker().toInt32(arg) : sema_.typeRelations().convert(arg, Type::doubleTy());
    }
    call->builtin = codeUnits ? Builtin::FromCharCode : Builtin::FromCodePoint;
    call->type = Type::string();
    return call;
  }
  if (!isString && (fn == "parseInt" || fn == "parseFloat")) {
    return sema_.globalFunctionChecker().checkGlobalFunction(call, fn, member->loc(), expected); // Number.parseInt is parseInt
  }
  static const std::pair<llvm::StringLiteral, Builtin> predicates[] = {
      {"isNaN", Builtin::IsNaN},
      {"isFinite", Builtin::IsFinite},
      {"isInteger", Builtin::IsInteger},
      {"isSafeInteger", Builtin::IsSafeInteger},
  };
  if (!isString) {
    for (const auto &[name, builtin] : predicates) {
      if (fn != name) {
        continue;
      }
      if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
        return call;
      }
      call->args[0] = sema_.mathChecker().numericArg(call->args[0]);
      call->builtin = builtin;
      call->type = Type::boolTy();
      return call;
    }
  }
  sema_.diags_.error(member->loc(), "property '" + fn + "' does not exist on '" +
                                  (isString ? "String" : "Number") + "'");
  for (Expr *&arg : call->args) {
    arg = sema_.exprChecker().checkExpr(arg);
  }

  return call;
}

// x.toString(), x.toString(radix) and x.toFixed(digits) of a number.
Expr *NumberMethods::checkNumberMethod(CallExpr *call, MemberExpr *member) {
  if (member->name == "toString" && call->args.size() == 1) {
    // The digits in base 2 to 36, as JavaScript writes them: `(255).toString(16)` is "ff".
    if (auto *lit = dyn_cast<IntLitExpr>(call->args[0]);
        lit && (lit->value < 2 || lit->value > 36)) {
      sema_.diags_.error(lit->loc(),
                   "toString() takes a radix from 2 to 36, found " + std::to_string(lit->value));
    }
    member->object = sema_.typeRelations().convert(member->object, Type::doubleTy());
    call->args[0] = sema_.memberChecker().checkPosition(call->args[0]);
    call->builtin = Builtin::NumToRadix;
    call->type = Type::string();
    return call;
  }
  if (member->name == "toString") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
      return call;
    }
    // `x.toString()` is `String(x)`.
    call->args.push_back(sema_.concatChecker().toStringOperand(member->object));
    call->builtin = Builtin::StringOf;
    call->type = Type::string();
    return call;
  }
  if (member->name == "toFixed") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    member->object = sema_.typeRelations().convert(member->object, Type::doubleTy());
    if (!call->args.empty()) {
      call->args[0] = sema_.memberChecker().checkPosition(call->args[0]);
    }
    call->builtin = Builtin::NumToFixed;
    call->type = Type::string();
    return call;
  }
  sema_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                  member->object->type->name() + "'");
  for (Expr *&arg : call->args) {
    arg = sema_.exprChecker().checkExpr(arg);
  }

  return call;
}

} // namespace tz
