#include "compiler/check/values/string_methods/string_methods.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/library/regex/string_regex_checker/string_regex_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

StringMethods::StringMethods(Checker &checker) : checker_(checker) {}

Expr *StringMethods::checkStringMethod(CallExpr *call, MemberExpr *member) {
  if (Expr *regex = checker_.stringRegexChecker().checkStringRegexMethod(call, member)) {
    return regex;
  }
  struct Method {
    llvm::StringRef name;
    Builtin builtin;
    unsigned minArgs, maxArgs;
    // The kinds of the arguments: 's' a string, 'p' a position or count,
    // '*' any value, turned into a string. The last one repeats.
    const char *args;
    const Type *ret;
  };
  constexpr unsigned kAny = ~0u;
  static const Method methods[] = {
      {"charCodeAt", Builtin::StrCharCodeAt, 1, 1, "p", Type::intTy()},
      {"charAt", Builtin::StrCharAt, 0, 1, "p", Type::string()},
      {"codePointAt", Builtin::StrCodePointAt, 1, 1, "p", Type::intTy()},
      {"at", Builtin::StrAt, 1, 1, "p", Type::string()},
      {"slice", Builtin::StrSlice, 0, 2, "p", Type::string()},
      {"substring", Builtin::StrSubstring, 1, 2, "p", Type::string()},
      {"indexOf", Builtin::StrIndexOf, 1, 2, "sp", Type::intTy()},
      {"lastIndexOf", Builtin::StrLastIndexOf, 1, 2, "sp", Type::intTy()},
      {"includes", Builtin::StrIncludes, 1, 2, "sp", Type::boolTy()},
      {"startsWith", Builtin::StrStartsWith, 1, 2, "sp", Type::boolTy()},
      {"endsWith", Builtin::StrEndsWith, 1, 2, "sp", Type::boolTy()},
      {"repeat", Builtin::StrRepeat, 1, 1, "p", Type::string()},
      {"toString", Builtin::StrToString, 0, 0, "", Type::string()},
      {"concat", Builtin::StrConcat, 0, kAny, "*", Type::string()},
      {"split", Builtin::StrSplit, 0, 2, "sp", Type::array(Type::string())},
      {"trim", Builtin::StrTrim, 0, 0, "", Type::string()},
      {"trimStart", Builtin::StrTrimStart, 0, 0, "", Type::string()},
      {"trimEnd", Builtin::StrTrimEnd, 0, 0, "", Type::string()},
      {"padStart", Builtin::StrPadStart, 1, 2, "ps", Type::string()},
      {"padEnd", Builtin::StrPadEnd, 1, 2, "ps", Type::string()},
      {"toUpperCase", Builtin::StrToUpperCase, 0, 0, "", Type::string()},
      {"toLowerCase", Builtin::StrToLowerCase, 0, 0, "", Type::string()},
      {"replace", Builtin::StrReplace, 2, 2, "ss", Type::string()},
      {"replaceAll", Builtin::StrReplaceAll, 2, 2, "ss", Type::string()},
  };

  const Method *method = nullptr;
  for (const Method &m : methods) {
    if (m.name == member->name) {
      method = &m;
    }
  }
  if (!method) {
    checker_.diags_.error(member->loc(),
                          "property '" + member->name + "' does not exist on type 'string'");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), method->minArgs,
                                             method->maxArgs)) {
    return call;
  }

  size_t kinds = std::strlen(method->args);
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    switch (method->args[std::min(i, kinds - 1)]) {
    case 's':
      arg = checker_.argumentChecker().checkArg(arg, Type::string());
      break;
    case 'p':
      arg = checker_.memberChecker().checkPosition(arg);
      break;
    default:
      arg = checker_.concatChecker().toStringOperand(checker_.exprChecker().checkExpr(arg));
      break;
    }
  }
  call->builtin = method->builtin;
  call->type = method->ret;

  if (method->builtin == Builtin::StrCharCodeAt) {
    // Out of range, JavaScript returns NaN, which only a double can hold.
    call->mayBeOutOfRange =
        !checker_.indexProofs().provesInBounds(member->object, call->args[0], /*allowEnd=*/false);
    call->type = call->mayBeOutOfRange ? Type::doubleTy() : Type::intTy();
  } else if (method->builtin == Builtin::StrCodePointAt) {
    // Out of range, JavaScript returns undefined.
    call->mayBeOutOfRange =
        !checker_.indexProofs().provesInBounds(member->object, call->args[0], /*allowEnd=*/false);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
  } else if (method->builtin == Builtin::StrAt) {
    call->mayBeOutOfRange = !checker_.indexProofs().provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::string()) : Type::string();
  }

  return call;
}

} // namespace tz
