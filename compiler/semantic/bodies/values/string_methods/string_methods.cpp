#include "compiler/semantic/bodies/values/string_methods/string_methods.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

namespace tz {

StringMethods::StringMethods(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.getExprChecker();
  this->indexProofs = &all.getIndexProofs();
  this->concatChecker = &all.getConcatChecker();
  this->memberChecker = &all.getMemberChecker();
  this->argumentChecker = &all.getArgumentChecker();
  this->stringRegexChecker = &all.getStringRegexChecker();
}

StringMethods::ResultStringMethod StringMethods::stringMethod(llvm::StringRef name) {
  constexpr unsigned any = ~0u;
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
      {"concat", Builtin::StrConcat, 0, any, "*", Type::string()},
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
    if (m.name == name) {
      method = &m;
    }
  }
  if (!method) {
    std::string message = ErrorTextStringMethod::missing(name);

    return err(StringMethodsStringMethodError{
        .code = StringMethodsStringMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(method);
}

Expr *StringMethods::checkStringMethod(CallExpr *call, MemberExpr *member) {
  if (Expr *regex = this->stringRegexChecker->checkStringRegexMethod(call, member)) {
    return regex;
  }
  ResultStringMethod found = StringMethods::stringMethod(member->name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    this->argumentChecker->checkArgsLoosely(call);
    return call;
  }
  const Method *method = found.value();
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, method->minArgs, method->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }

  size_t kinds = std::strlen(method->args);
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    switch (method->args[std::min(i, kinds - 1)]) {
    case 's':
      arg = this->argumentChecker->checkArg(arg, Type::string());
      break;
    case 'p':
      arg = this->memberChecker->checkPosition(arg);
      break;
    default:
      arg = this->concatChecker->toStringOperand(this->exprChecker->checkExpr(arg));
      break;
    }
  }
  call->target = BuiltinCall{.builtin = method->builtin};
  call->type = method->ret;

  if (method->builtin == Builtin::StrRepeat) {
    std::string counted = ErrorTextStringMethod::repeatOf();
    IndexProofs::ResultBetweenProven between =
        this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, counted);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
  } else if (method->builtin == Builtin::StrCharCodeAt) {
    call->mayBeOutOfRange =
        !this->indexProofs->provesInBounds(member->object, call->args[0], false);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
  } else if (method->builtin == Builtin::StrCodePointAt) {
    call->mayBeOutOfRange =
        !this->indexProofs->provesInBounds(member->object, call->args[0], false);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
  } else if (method->builtin == Builtin::StrAt) {
    call->mayBeOutOfRange = !this->indexProofs->provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::string()) : Type::string();
  }

  return call;
}

}
