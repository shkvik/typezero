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

namespace tz {

namespace {

constexpr char kStringArg = 's';
constexpr char kPositionArg = 'p';

}

StringMethods::StringMethods(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->concatChecker = &all.concatChecker;
  this->memberChecker = &all.memberChecker;
  this->argumentChecker = &all.argumentChecker;
  this->stringRegexChecker = &all.stringRegexChecker;
}

const Type *StringMethods::resultType(StringResult result) const {
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

  return Type::error();
}

StringMethods::ResultStringMethod StringMethods::checkMethodName(llvm::StringRef name) const {
  const auto *method = StringMethodTable::find(name);
  bool isCallable = method != nullptr && StringMethodTable::findCall(*method) != nullptr;
  if (!isCallable) {
    auto message = ErrorTextStringMethod::missing(name);

    return err(StringMethodsStringMethodError{
        .code = StringMethodsStringMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(method);
}

Expr *StringMethods::checkStringMethod(CallExpr *call, MemberExpr *member) {
  auto *regex = this->stringRegexChecker->checkStringRegexMethod(call, member);
  if (regex != nullptr) {
    return regex;
  }
  auto found = this->checkMethodName(member->name);
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  const StringMethodInfo *method = found.value();
  const auto *stringCall = StringMethodTable::findCall(*method);
  auto arity = this->argumentChecker->checkArity(call, stringCall->minArgs, stringCall->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }

  this->walkStringArgs(call, stringCall->args);
  call->target = BuiltinCall{.builtin = stringCall->builtin};
  call->type = this->resultType(stringCall->result);
  this->walkMethodRange(call, member, method->method);

  return call;
}

void StringMethods::walkStringArgs(CallExpr *call, llvm::StringRef argKinds) {
  size_t lastKind = argKinds.size() - 1;
  for (size_t index = 0; index < call->args.size(); ++index) {
    char kind = argKinds[std::min(index, lastKind)];
    call->args[index] = this->checkStringArg(call->args[index], kind);
  }
}

Expr *StringMethods::checkStringArg(Expr *arg, char kind) {
  if (kind == kStringArg) {
    return this->argumentChecker->checkArg(arg, Type::string());
  }
  if (kind == kPositionArg) {
    return this->memberChecker->checkPosition(arg);
  }
  auto *checked = this->exprChecker->checkExpr(arg);

  return this->concatChecker->toStringOperand(checked);
}

void StringMethods::walkMethodRange(CallExpr *call, const MemberExpr *member, StringMethod method) {
  bool isCodeAt = method == StringMethod::CHAR_CODE_AT || method == StringMethod::CODE_POINT_AT;
  if (method == StringMethod::REPEAT) {
    this->walkRepeatCount(call);
    return;
  }
  if (isCodeAt) {
    call->mayBeOutOfRange = !this->indexProofs->canProveInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
    return;
  }
  if (method == StringMethod::AT) {
    call->mayBeOutOfRange = !this->indexProofs->canProveAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::string()) : Type::string();
  }
}

void StringMethods::walkRepeatCount(const CallExpr *call) {
  auto counted = ErrorTextStringMethod::repeatOf();
  auto between = this->indexProofs->requireBetween(call->args[0], 0, std::nullopt, counted);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(call->args[0]->loc(), between.error().message);
  }
}

}
