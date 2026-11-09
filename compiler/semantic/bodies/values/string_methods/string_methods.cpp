#include "compiler/semantic/bodies/values/string_methods/string_methods.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

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

const Type *StringMethods::resultType(StringResult result) {
  switch (result) {
    case StringResult::INT:
      return Type::intTy();
    case StringResult::BOOL:
      return Type::boolTy();
    case StringResult::STRING:
      return Type::string();
    case StringResult::STRINGS:
      return Type::array(Type::string());
  }
  llvm_unreachable("unknown string result");
}

StringMethods::ResultStringMethod StringMethods::stringMethod(llvm::StringRef name) {
  const StringMethodInfo *method = StringMethodTable::find(name);
  const StringCall *stringCall = nullptr;
  if (method != nullptr) {
    stringCall = StringMethodTable::findCall(*method);
  }
  if (stringCall == nullptr) {
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
  const StringMethodInfo *method = found.value();
  const StringCall *stringCall = StringMethodTable::findCall(*method);
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, stringCall->minArgs, stringCall->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }

  llvm::StringRef argKinds = stringCall->args;
  size_t kinds = argKinds.size();
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    switch (argKinds[std::min(i, kinds - 1)]) {
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
  call->target = BuiltinCall{.builtin = stringCall->builtin};
  call->type = StringMethods::resultType(stringCall->result);

  if (method->method == StringMethod::REPEAT) {
    std::string counted = ErrorTextStringMethod::repeatOf();
    IndexProofs::ResultBetweenProven between =
        this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, counted);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
    }
  } else if (method->method == StringMethod::CHAR_CODE_AT) {
    call->mayBeOutOfRange =
        !this->indexProofs->provesInBounds(member->object, call->args[0], false);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
  } else if (method->method == StringMethod::CODE_POINT_AT) {
    call->mayBeOutOfRange =
        !this->indexProofs->provesInBounds(member->object, call->args[0], false);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
  } else if (method->method == StringMethod::AT) {
    call->mayBeOutOfRange = !this->indexProofs->provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::string()) : Type::string();
  }

  return call;
}

}
