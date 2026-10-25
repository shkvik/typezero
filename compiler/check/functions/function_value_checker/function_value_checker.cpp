#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// How a callback of a built-in method is called, for messages.
const char *callbackArgs(llvm::StringRef method) {
  if (method == "reduce" || method == "reduceRight") {
    return "(accumulator, value, index, array)";
  }
  if (method == "sort") {
    return "(a, b)";
  }

  return "(value, index, array)";
}

const char *ordinal(size_t i) {
  static const char *const names[] = {"first", "second", "third", "fourth", "fifth"};

  return i < 5 ? names[i] : "next";
}

// What the arguments of a callback are, by position.
const char *callbackArgName(llvm::StringRef method, size_t i) {
  bool reduce = method == "reduce" || method == "reduceRight";
  if (reduce) {
    return i == 0 ? "accumulator" : i == 1 ? "value" : i == 2 ? "index" : "array";
  }

  return i == 0 ? "value" : i == 1 ? "index" : "array";
}

} // namespace

FunctionValueChecker::FunctionValueChecker(Checker &checker) : checker_(checker) {}

Expr *FunctionValueChecker::adaptFunction(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  if (!checker_.nestedFunctions().namedFunction(expr) || from->hasRest() || to->hasRest()) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> params = from->params(), wanted = to->params();
  for (size_t i = 0; i < wanted.size() && i < params.size(); ++i) {
    if (params[i] != wanted[i] && !(params[i]->isOptional() && params[i]->inner() == wanted[i])) {
      return nullptr;
    }
  }
  // The parameters callers do not pass must be ones it can go without.
  if (wanted.size() < from->minArgs()) {
    return nullptr;
  }
  if (!to->ret()->isVoid() && from->ret() != to->ret()) {
    return nullptr;
  }
  auto *cast = checker_.ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
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
        {"isNaN", 1},     {"isFinite", 1},
        {"Boolean", 1},   {"encodeURIComponent", 1},
        {"encodeURI", 1}, {"decodeURIComponent", 1},
        {"decodeURI", 1}};
    for (const auto &[global, count] : globals) {
      if (checker_.callChecker().isGlobalName(n, global)) {
        name = global;
        maxArgs = count;
      }
    }
    // `xs.map(ok)` makes a Result of each element.
    if (llvm::StringRef result = checker_.resultTypes().neverthrowName(n->name);
        result == "ok" || result == "err") {
      name = n->name;
      maxArgs = 1;
    }
  } else if (auto *m = dyn_cast<MemberExpr>(arg)) {
    name = m->name;
    if (checker_.callChecker().isGlobalName(m->object, "Math")) {
      static const llvm::StringLiteral two[] = {"pow", "atan2", "imul"};
      static const llvm::StringLiteral any[] = {"min", "max", "hypot"};
      static const llvm::StringLiteral one[] = {
          "floor", "ceil",  "round", "trunc", "abs",   "sign",  "sqrt",   "cbrt", "exp",  "expm1",
          "log",   "log1p", "log2",  "log10", "sin",   "cos",   "tan",    "asin", "acos", "atan",
          "sinh",  "cosh",  "tanh",  "asinh", "acosh", "atanh", "fround", "clz32"};
      if (llvm::is_contained(two, name)) {
        maxArgs = 2;
      } else if (llvm::is_contained(any, name)) {
        maxArgs = kAny;
      } else if (llvm::is_contained(one, name)) {
        maxArgs = 1;
      } else if (name != "random") {
        name = {};
      }
    } else if (checker_.callChecker().isGlobalName(m->object, "Number")) {
      if (name == "parseInt") {
        maxArgs = 2;
      } else if (name == "parseFloat" || name == "isNaN" || name == "isFinite" ||
                 name == "isInteger" || name == "isSafeInteger") {
        maxArgs = 1;
      } else {
        name = {};
      }
    } else if (checker_.callChecker().isGlobalName(m->object, "String")) {
      if (name == "fromCharCode" || name == "fromCodePoint") {
        maxArgs = kAny;
      } else {
        name = {};
      }
    } else if (checker_.callChecker().isGlobalName(m->object, "console") &&
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
  if (count > primary) {
    // `map(parseInt)`: parseInt would take the index as its radix.
    std::string qualified = name.str();
    if (auto *m = dyn_cast<MemberExpr>(arg)) {
      qualified = cast<NameExpr>(m->object)->name.str() + "." + qualified;
    }
    checker_.diags_.error(arg->loc(), "'" + qualified + "' would also get the " +
                                          callbackArgName(method, primary) + " as its " +
                                          ordinal(primary) + " argument: '" + method +
                                          "' calls it with " + callbackArgs(method) +
                                          "; pass only what it needs: 'x => " + qualified + "(x)'");
    count = primary;
  }
  auto *fn = checker_.ctx_.make<FunctionDecl>(arg->loc(), name);
  fn->isExpression = fn->isArrow = fn->concise = true;
  std::vector<Expr *> args;
  for (size_t i = 0; i < count; ++i) {
    Param param;
    param.name = checker_.ctx_.hiddenName();
    param.loc = arg->loc();
    fn->params.push_back(param);
    args.push_back(checker_.ctx_.make<NameExpr>(arg->loc(), param.name));
  }
  auto *call = checker_.ctx_.make<CallExpr>(arg->loc(), arg, std::move(args));
  fn->body = checker_.ctx_.make<BlockStmt>(arg->loc());
  if (returnsNothing) {
    fn->body->body.push_back(checker_.ctx_.make<ExprStmt>(arg->loc(), call));
  } else {
    fn->body->body.push_back(checker_.ctx_.make<ReturnStmt>(arg->loc(), call));
  }

  return checker_.ctx_.make<FunctionExpr>(arg->loc(), fn);
}

bool FunctionValueChecker::asyncCallbackProblem(const Expr *arg, const Checker::Signature &expected,
                                                llvm::StringRef method) {
  const Type *type = arg->type;
  if (!type->isFunction() || !type->ret()->isPromise() || !expected.ret ||
      expected.ret->isPromise() || expected.voidResult || expected.ret->isError()) {
    return false;
  }
  std::string why;
  if (method == "replace" || method == "replaceAll") {
    why = "; JavaScript would insert \"[object Promise]\" for each match: work out the "
          "replacements first, then replace with a function that is not async";
  } else if (expected.ret->isBool()) {
    why = "; a Promise is always truthy, so JavaScript would take every element as passing: "
          "await the tests first, e.g. 'const passed = await Promise.all(xs.map(async x => "
          "...))', then test 'passed[i]'";
  } else {
    why = "; drop 'async', or await what the function needs before the call";
  }
  checker_.diags_.error(arg->loc(), "'" + method + "' uses what the function returns as '" +
                                        expected.ret->name() +
                                        "', but an async function returns a Promise" + why);

  return true;
}

} // namespace tz
