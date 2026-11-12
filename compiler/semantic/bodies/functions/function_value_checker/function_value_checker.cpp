#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/program/library/builtin_names/function_value_table/function_value_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/functions/callback_checker/callback_checker_errors.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

FunctionValueChecker::FunctionValueChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->resultTypes = &all.resultTypes;
  this->nestedFunctions = &all.nestedFunctions;
}

Expr *FunctionValueChecker::adaptFunction(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  if (this->nestedFunctions->namedFunction(expr) == nullptr || from->hasRest() || to->hasRest()) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> params = from->params();
  llvm::ArrayRef<const Type *> wanted = to->params();
  size_t shared = std::min(params.size(), wanted.size());
  for (size_t index = 0; index < shared; ++index) {
    if (!this->isParamFitting(params[index], wanted[index])) {
      return nullptr;
    }
  }
  if (wanted.size() < from->minArgs()) {
    return nullptr;
  }
  if (!to->ret()->isVoid() && from->ret() != to->ret()) {
    return nullptr;
  }
  auto *castExpr = this->state.getAst().make<CastExpr>(expr->loc(), expr, std::nullopt);
  castExpr->type = to;

  return castExpr;
}

bool FunctionValueChecker::isParamFitting(const Type *param, const Type *wanted) const {
  bool isOptionalWanted = param->isOptional() && param->inner() == wanted;
  bool isUnknownTaken = param->nonOptional()->isUnknown() && !wanted->isVoid();

  return param == wanted || isOptionalWanted || isUnknownTaken;
}

FunctionExpr *FunctionValueChecker::builtinFunctionValue(Expr *arg, const Signature &expected,
                                                         unsigned primary, llvm::StringRef method) {
  auto shape = this->functionValueShape(arg);
  if (!shape.has_value()) {
    return nullptr;
  }
  size_t offered = std::min<size_t>(shape->maxArgs, expected.params.size());
  auto matched = this->checkExtraArguments(arg, shape->name, method, offered, primary);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), matched.error().message);
    return this->forwardingFunction(arg, *shape, primary);
  }

  return this->forwardingFunction(arg, *shape, matched.value());
}

std::optional<FunctionValueChecker::FunctionValueShape>
FunctionValueChecker::functionValueShape(const Expr *arg) const {
  auto *named = dyn_cast<NameExpr>(arg);
  if (named != nullptr) {
    return this->globalFunctionShape(named);
  }
  auto *member = dyn_cast<MemberExpr>(arg);
  if (member != nullptr) {
    return this->staticFunctionShape(member);
  }

  return std::nullopt;
}

std::optional<FunctionValueChecker::FunctionValueShape>
FunctionValueChecker::globalFunctionShape(const NameExpr *named) const {
  auto result = this->resultTypes->resultName(named->name);
  if (result == GlobalName::OK || result == GlobalName::ERR) {
    return FunctionValueShape{
        .name = named->name,
        .maxArgs = 1,
        .isReturningNothing = false,
    };
  }
  auto global = this->namedGlobal(named);
  const FunctionValueGlobalInfo *info =
      global.has_value() ? FunctionValueTable::findGlobal(*global) : nullptr;
  if (info == nullptr) {
    return std::nullopt;
  }

  return FunctionValueShape{
      .name = GlobalNameTable::getName(info->global),
      .maxArgs = info->maxArgs,
      .isReturningNothing = false,
  };
}

std::optional<FunctionValueChecker::FunctionValueShape>
FunctionValueChecker::staticFunctionShape(const MemberExpr *member) const {
  auto owner = this->namedGlobal(member->object);
  const FunctionValueStaticInfo *info =
      owner.has_value() ? FunctionValueTable::findStatic(*owner, member->name) : nullptr;
  if (info == nullptr) {
    return std::nullopt;
  }

  return FunctionValueShape{
      .name = member->name,
      .maxArgs = info->maxArgs,
      .isReturningNothing = info->isReturningNothing,
  };
}

std::optional<GlobalName> FunctionValueChecker::namedGlobal(const Expr *expr) const {
  auto *named = dyn_cast<NameExpr>(expr);
  if (named == nullptr) {
    return std::nullopt;
  }
  auto global = GlobalNameTable::find(named->name);
  if (!global.has_value() || !this->callChecker->isGlobalName(named, *global)) {
    return std::nullopt;
  }

  return global;
}

FunctionExpr *FunctionValueChecker::forwardingFunction(Expr *arg, const FunctionValueShape &shape,
                                                       size_t count) {
  auto *functionDecl = this->state.getAst().make<FunctionDecl>(arg->loc(), shape.name);
  functionDecl->place = FunctionPlace::EXPRESSION;
  functionDecl->syntax = ArrowFunction{.concise = true};
  auto args = std::vector<Expr *>();
  for (size_t index = 0; index < count; ++index) {
    auto param = Param{
        .name = this->state.getAst().hiddenName(),
        .loc = arg->loc(),
    };
    functionDecl->params.push_back(param);
    auto *forwarded = this->state.getAst().make<NameExpr>(arg->loc(), param.name);
    args.push_back(forwarded);
  }
  auto *call = this->state.getAst().make<CallExpr>(arg->loc(), arg, std::move(args));
  functionDecl->body = this->state.getAst().make<BlockStmt>(arg->loc());
  auto *statement = this->forwardingStatement(arg, call, shape);
  functionDecl->body->body.push_back(statement);

  return this->state.getAst().make<FunctionExpr>(arg->loc(), functionDecl);
}

Stmt *FunctionValueChecker::forwardingStatement(const Expr *arg, CallExpr *call,
                                                const FunctionValueShape &shape) {
  if (shape.isReturningNothing) {
    return this->state.getAst().make<ExprStmt>(arg->loc(), call);
  }

  return this->state.getAst().make<ReturnStmt>(arg->loc(), call);
}

FunctionValueChecker::ResultArgumentsMatch
FunctionValueChecker::checkExtraArguments(const Expr *arg, llvm::StringRef name,
                                          llvm::StringRef method, size_t count,
                                          unsigned primary) const {
  if (count <= primary) {
    return ok(count);
  }
  auto qualified = this->qualifiedName(arg, name);
  auto argument = ErrorTextCallbackArg::argName(method, primary);
  auto ordinal = ErrorTextCallbackArg::position(primary);
  auto passes = ErrorTextCallbackArg::argsShape(method);
  auto message = ErrorTextExtraArguments::passed(qualified, argument, ordinal, method, passes);

  return err(FunctionValueCheckerExtraArgumentsError{
      .code = FunctionValueCheckerExtraArgumentsErrorCode::MORE_ARGUMENTS,
      .message = message,
  });
}

std::string FunctionValueChecker::qualifiedName(const Expr *arg, llvm::StringRef name) const {
  auto *member = dyn_cast<MemberExpr>(arg);
  if (member == nullptr) {
    return name.str();
  }
  auto *owner = cast<NameExpr>(member->object);

  return owner->name.str() + "." + name.str();
}

FunctionValueChecker::ResultAsyncCallbackOk
FunctionValueChecker::checkAsyncCallback(const Expr *arg, const Signature &expected,
                                         llvm::StringRef method) const {
  const Type *type = arg->type;
  bool isPromised = type->isFunction() && type->ret()->isPromise() && expected.ret != nullptr &&
                    !expected.ret->isPromise() && !expected.voidResult && !expected.ret->isError();
  const auto *stringMethod = StringMethodTable::find(method);
  bool isReplacing = stringMethod != nullptr && (stringMethod->method == StringMethod::REPLACE ||
                                                 stringMethod->method == StringMethod::REPLACE_ALL);
  if (isPromised && isReplacing) {
    auto message = ErrorTextAsyncCallback::replace(method, expected.ret->name());

    return err(FunctionValueCheckerAsyncCallbackError{
        .code = FunctionValueCheckerAsyncCallbackErrorCode::REPLACE_GETS_PROMISE,
        .message = message,
    });
  }
  if (isPromised && expected.ret->isBool()) {
    auto message = ErrorTextAsyncCallback::test(method, expected.ret->name());

    return err(FunctionValueCheckerAsyncCallbackError{
        .code = FunctionValueCheckerAsyncCallbackErrorCode::TEST_GETS_PROMISE,
        .message = message,
    });
  }
  if (isPromised) {
    auto message = ErrorTextAsyncCallback::value(method, expected.ret->name());

    return err(FunctionValueCheckerAsyncCallbackError{
        .code = FunctionValueCheckerAsyncCallbackErrorCode::VALUE_GETS_PROMISE,
        .message = message,
    });
  }

  return ok(arg);
}

}
