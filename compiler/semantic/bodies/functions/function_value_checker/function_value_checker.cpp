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
  if (!this->nestedFunctions->namedFunction(expr) || from->hasRest() || to->hasRest()) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> params = from->params();
  llvm::ArrayRef<const Type *> wanted = to->params();
  for (size_t i = 0; i < wanted.size() && i < params.size(); ++i) {
    if (params[i] != wanted[i] && !(params[i]->isOptional() && params[i]->inner() == wanted[i]) &&
        !(params[i]->nonOptional()->isUnknown() && !wanted[i]->isVoid())) {
      return nullptr;
    }
  }
  if (wanted.size() < from->minArgs()) {
    return nullptr;
  }
  if (!to->ret()->isVoid() && from->ret() != to->ret()) {
    return nullptr;
  }
  auto *cast = this->state.getAst().make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;

  return cast;
}

FunctionExpr *FunctionValueChecker::builtinFunctionValue(Expr *arg, const Signature &expected,
                                                         unsigned primary, llvm::StringRef method) {
  std::optional<FunctionValueShape> shape = this->functionValueShape(arg);
  if (!shape.has_value()) {
    return nullptr;
  }
  size_t count = std::min<size_t>(shape->maxArgs, expected.params.size());
  ResultArgumentsMatch matched =
      FunctionValueChecker::extraArguments(arg, shape->name, method, count, primary);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), matched.error().message);
    count = primary;
  }

  return this->forwardingFunction(arg, *shape, count);
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
  std::optional<GlobalName> result = this->resultTypes->resultName(named->name);
  if (result == GlobalName::OK || result == GlobalName::ERR) {
    return FunctionValueShape{
        .name = named->name,
        .maxArgs = 1,
        .returnsNothing = false,
    };
  }
  std::optional<GlobalName> global = this->namedGlobal(named);
  const FunctionValueGlobalInfo *info =
      global.has_value() ? FunctionValueTable::findGlobal(*global) : nullptr;
  if (info == nullptr) {
    return std::nullopt;
  }

  return FunctionValueShape{
      .name = GlobalNameTable::getName(info->global),
      .maxArgs = info->maxArgs,
      .returnsNothing = false,
  };
}

std::optional<FunctionValueChecker::FunctionValueShape>
FunctionValueChecker::staticFunctionShape(const MemberExpr *member) const {
  std::optional<GlobalName> owner = this->namedGlobal(member->object);
  const FunctionValueStaticInfo *info =
      owner.has_value() ? FunctionValueTable::findStatic(*owner, member->name) : nullptr;
  if (info == nullptr) {
    return std::nullopt;
  }

  return FunctionValueShape{
      .name = member->name,
      .maxArgs = info->maxArgs,
      .returnsNothing = info->returnsNothing,
  };
}

std::optional<GlobalName> FunctionValueChecker::namedGlobal(const Expr *expr) const {
  auto *named = dyn_cast<NameExpr>(expr);
  if (named == nullptr) {
    return std::nullopt;
  }
  std::optional<GlobalName> global = GlobalNameTable::find(named->name);
  if (!global.has_value() || !this->callChecker->isGlobalName(named, *global)) {
    return std::nullopt;
  }

  return global;
}

FunctionExpr *FunctionValueChecker::forwardingFunction(Expr *arg, const FunctionValueShape &shape,
                                                       size_t count) {
  auto *fn = this->state.getAst().make<FunctionDecl>(arg->loc(), shape.name);
  fn->place = FunctionPlace::EXPRESSION;
  fn->syntax = ArrowFunction{.concise = true};
  std::vector<Expr *> args;
  for (size_t i = 0; i < count; ++i) {
    Param param;
    param.name = this->state.getAst().hiddenName();
    param.loc = arg->loc();
    fn->params.push_back(param);
    args.push_back(this->state.getAst().make<NameExpr>(arg->loc(), param.name));
  }
  auto *call = this->state.getAst().make<CallExpr>(arg->loc(), arg, std::move(args));
  fn->body = this->state.getAst().make<BlockStmt>(arg->loc());
  if (shape.returnsNothing) {
    fn->body->body.push_back(this->state.getAst().make<ExprStmt>(arg->loc(), call));
  } else {
    fn->body->body.push_back(this->state.getAst().make<ReturnStmt>(arg->loc(), call));
  }

  return this->state.getAst().make<FunctionExpr>(arg->loc(), fn);
}

FunctionValueChecker::ResultArgumentsMatch
FunctionValueChecker::extraArguments(const Expr *arg, llvm::StringRef name, llvm::StringRef method,
                                     size_t count, unsigned primary) {
  if (count > primary) {
    std::string qualified = name.str();
    auto *m = dyn_cast<MemberExpr>(arg);
    if (m) {
      qualified = cast<NameExpr>(m->object)->name.str() + "." + qualified;
    }
    std::string argument = ErrorTextCallbackArg::argName(method, primary);
    std::string ordinal = ErrorTextCallbackArg::position(primary);
    std::string passes = ErrorTextCallbackArg::argsShape(method);
    std::string message =
        ErrorTextExtraArguments::passed(qualified, argument, ordinal, method, passes);

    return err(FunctionValueCheckerExtraArgumentsError{
        .code = FunctionValueCheckerExtraArgumentsErrorCode::MORE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(count);
}

FunctionValueChecker::ResultAsyncCallbackOk
FunctionValueChecker::asyncCallback(const Expr *arg, const Signature &expected,
                                    llvm::StringRef method) const {
  const Type *type = arg->type;
  bool promised = type->isFunction() && type->ret()->isPromise() && expected.ret &&
                  !expected.ret->isPromise() && !expected.voidResult && !expected.ret->isError();
  const StringMethodInfo *stringMethod = StringMethodTable::find(method);
  bool replaces = stringMethod != nullptr && (stringMethod->method == StringMethod::REPLACE ||
                                              stringMethod->method == StringMethod::REPLACE_ALL);
  if (promised && replaces) {
    std::string message = ErrorTextAsyncCallback::replace(method, expected.ret->name());

    return err(FunctionValueCheckerAsyncCallbackError{
        .code = FunctionValueCheckerAsyncCallbackErrorCode::REPLACE_GETS_PROMISE,
        .message = message,
    });
  }
  if (promised && expected.ret->isBool()) {
    std::string message = ErrorTextAsyncCallback::test(method, expected.ret->name());

    return err(FunctionValueCheckerAsyncCallbackError{
        .code = FunctionValueCheckerAsyncCallbackErrorCode::TEST_GETS_PROMISE,
        .message = message,
    });
  }
  if (promised) {
    std::string message = ErrorTextAsyncCallback::value(method, expected.ret->name());

    return err(FunctionValueCheckerAsyncCallbackError{
        .code = FunctionValueCheckerAsyncCallbackErrorCode::VALUE_GETS_PROMISE,
        .message = message,
    });
  }

  return ok(arg);
}

}
