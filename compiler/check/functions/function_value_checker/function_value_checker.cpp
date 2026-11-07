#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

const char *FunctionValueChecker::callbackArgs(llvm::StringRef method) {
  if (method == "reduce" || method == "reduceRight") {
    return "(accumulator, value, index, array)";
  }
  if (method == "sort") {
    return "(a, b)";
  }

  return "(value, index, array)";
}

const char *FunctionValueChecker::ordinal(size_t i) {
  static const char *const names[] = {"first", "second", "third", "fourth", "fifth"};

  return i < 5 ? names[i] : "next";
}

const char *FunctionValueChecker::callbackArgName(llvm::StringRef method, size_t i) {
  bool reduce = method == "reduce" || method == "reduceRight";
  if (reduce) {
    return i == 0 ? "accumulator" : i == 1 ? "value" : i == 2 ? "index" : "array";
  }

  return i == 0 ? "value" : i == 1 ? "index" : "array";
}

FunctionValueChecker::FunctionValueChecker(Checker &owner)
    : checker(owner) {}

Expr *FunctionValueChecker::adaptFunction(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  if (!this->checker.nestedFunctions().namedFunction(expr) || from->hasRest() || to->hasRest()) {
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
  auto *cast = this->checker.ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;

  return cast;
}

FunctionExpr *FunctionValueChecker::builtinFunctionValue(Expr *arg,
                                                         const Checker::Signature &expected,
                                                         unsigned primary, llvm::StringRef method) {
  constexpr unsigned kAny = ~0u;
  llvm::StringRef name;
  unsigned maxArgs = 0;
  bool returnsNothing = false;
  if (auto *n = dyn_cast<NameExpr>(arg)) {
    static const std::pair<llvm::StringLiteral, unsigned> globals[] = {
        {"String", 1},    {"Number", 1},
        {"parseInt", 2},  {"parseFloat", 1},
        {"Boolean", 1},   {"encodeURIComponent", 1},
        {"encodeURI", 1}, {"decodeURIComponent", 1},
        {"decodeURI", 1}};
    for (const auto &[global, count] : globals) {
      if (this->checker.callChecker().isGlobalName(n, global)) {
        name = global;
        maxArgs = count;
      }
    }
    if (llvm::StringRef result = this->checker.resultTypes().resultName(n->name);
        result == "ok" || result == "err") {
      name = n->name;
      maxArgs = 1;
    }
  } else if (auto *m = dyn_cast<MemberExpr>(arg)) {
    name = m->name;
    if (this->checker.callChecker().isGlobalName(m->object, "Math")) {
      static const llvm::StringLiteral two[] = {"pow", "atan2", "imul"};
      static const llvm::StringLiteral any[] = {"min", "max", "hypot"};
      static const llvm::StringLiteral one[] = {
          "floor", "ceil",  "round", "trunc", "abs",   "sign",  "sqrt",   "cbrt", "exp",  "expm1",
          "log",   "log1p", "log2",  "log10", "sin",   "cos",   "tan",    "asin", "acos", "atan",
          "sinh",  "cosh",  "tanh",  "asinh", "acosh", "atanh", "fround", "clz32"};
      if (name == "clamp") {
        maxArgs = 3;
      } else if (llvm::is_contained(two, name)) {
        maxArgs = 2;
      } else if (llvm::is_contained(any, name)) {
        maxArgs = kAny;
      } else if (llvm::is_contained(one, name)) {
        maxArgs = 1;
      } else if (name != "random") {
        name = {};
      }
    } else if (this->checker.callChecker().isGlobalName(m->object, "Number")) {
      if (name == "parseInt") {
        maxArgs = 2;
      } else if (name == "parseFloat" || name == "isInteger" || name == "isSafeInteger") {
        maxArgs = 1;
      } else {
        name = {};
      }
    } else if (this->checker.callChecker().isGlobalName(m->object, "String")) {
      if (name == "fromCharCode" || name == "fromCodePoint") {
        maxArgs = kAny;
      } else {
        name = {};
      }
    } else if (this->checker.callChecker().isGlobalName(m->object, "console") &&
               (name == "log" || name == "info" || name == "debug" || name == "error" ||
                name == "warn")) {
      maxArgs = kAny;
      returnsNothing = true;
    } else {
      name = {};
    }
  }
  if (name.empty()) {
    return nullptr;
  }

  size_t count = std::min<size_t>(maxArgs, expected.params.size());
  ResultArgumentsMatch matched =
      FunctionValueChecker::extraArguments(arg, name, method, count, primary);
  if (matched.isErr()) {
    this->checker.diags_.error(arg->loc(), matched.error().message);
    count = primary;
  }
  auto *fn = this->checker.ctx_.make<FunctionDecl>(arg->loc(), name);
  fn->place = FunctionPlace::EXPRESSION;
  fn->syntax = ArrowFunction{.concise = true};
  std::vector<Expr *> args;
  for (size_t i = 0; i < count; ++i) {
    Param param;
    param.name = this->checker.ctx_.hiddenName();
    param.loc = arg->loc();
    fn->params.push_back(param);
    args.push_back(this->checker.ctx_.make<NameExpr>(arg->loc(), param.name));
  }
  auto *call = this->checker.ctx_.make<CallExpr>(arg->loc(), arg, std::move(args));
  fn->body = this->checker.ctx_.make<BlockStmt>(arg->loc());
  if (returnsNothing) {
    fn->body->body.push_back(this->checker.ctx_.make<ExprStmt>(arg->loc(), call));
  } else {
    fn->body->body.push_back(this->checker.ctx_.make<ReturnStmt>(arg->loc(), call));
  }

  return this->checker.ctx_.make<FunctionExpr>(arg->loc(), fn);
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
    const char *argument = FunctionValueChecker::callbackArgName(method, primary);
    const char *ordinal = FunctionValueChecker::ordinal(primary);
    const char *passes = FunctionValueChecker::callbackArgs(method);
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
FunctionValueChecker::asyncCallback(const Expr *arg, const Checker::Signature &expected,
                                    llvm::StringRef method) const {
  const Type *type = arg->type;
  bool promised = type->isFunction() && type->ret()->isPromise() && expected.ret &&
                  !expected.ret->isPromise() && !expected.voidResult && !expected.ret->isError();
  bool replaces = method == "replace" || method == "replaceAll";
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
