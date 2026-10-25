#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

ArgumentChecker::ArgumentChecker(Sema &sema) : sema_(sema) {}

Expr *ArgumentChecker::checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs,
                           bool hasRest, llvm::SMLoc loc, const std::vector<bool> *checked) {
  // `...t` of a tuple in a variable, and of an argument list, as checkCall
  // does for the other calls.
  if (!checked) {
    sema_.argListChecker().expandListSpreads(call->args, call->spread);
  }
  size_t fixed = params.size() - (hasRest ? 1 : 0);
  // In generic code, a rest parameter of a list of types (`...args: A`):
  // spreads of lists (docs/generics.md, section 9).
  if (hasRest && (params.back()->isListParam() || spreadsLists(params.back()))) {
    for (size_t i = 0; i < call->args.size() && i < fixed; ++i) {
      if (call->spreads(i) && sema_.callChecker().rejectSpread(call)) {
        return call;
      }
    }
    if (!checkArity(call, loc, static_cast<unsigned>(minArgs), ~0u)) {
      return call;
    }
    for (size_t i = 0; i < fixed; ++i) {
      bool given = checked && i < checked->size() && (*checked)[i];
      call->args[i] = given ? sema_.typeRelations().coerce(call->args[i], params[i]) : checkArg(call->args[i], params[i]);
    }
    sema_.argListChecker().checkListArguments(call, fixed, params.back(), loc, checked);

    return call;
  }
  for (size_t i = 0; i < call->args.size() && i < (hasRest ? fixed : call->args.size()); ++i) {
    if (call->spreads(i) && sema_.callChecker().rejectSpread(call)) {
      return call;
    }
  }
  if (!checkArity(call, loc, static_cast<unsigned>(minArgs),
                  hasRest ? ~0u : static_cast<unsigned>(params.size()))) {
    return call;
  }
  auto isChecked = [&](size_t i) { return checked && i < checked->size() && (*checked)[i]; };
  Expr *checkedRest = nullptr;
  const Type *listRest = hasRest && (params.back()->isTuple() || params.back()->isListParam())
                             ? params.back()
                             : nullptr;
  if (listRest && call->args.size() == fixed + 1 && call->spreads(fixed)) {
    // A list of parameters, `...args: A` (docs/generics.md, section 9): a
    // list of its type spread there goes as it is.
    Expr *&arg = call->args[fixed];
    if (!isChecked(fixed)) {
      arg = sema_.exprChecker().checkExpr(arg, listRest);
    }
    if (arg->type != listRest && !arg->type->isError() && !listRest->isError()) {
      sema_.diags_.error(arg->loc(),
                   "cannot spread '" + arg->type->name() + "' into '" + listRest->name() + "'");
    }
    checkedRest = arg;
    call->spread.clear();
  } else if (hasRest) {
    // The arguments for the rest parameter become one array, as `f(1, ...a)`
    // passes `[1, ...a]`.
    llvm::SMLoc restLoc = call->args.size() > fixed ? call->args[fixed]->loc() : call->loc();
    auto *rest = sema_.ctx_.make<ArrayLitExpr>(restLoc);
    const Type *restType = params.back();
    for (size_t i = fixed; i < call->args.size(); ++i) {
      Expr *arg = call->args[i];
      // Checked already: the array has the elements as they are.
      if (isChecked(i)) {
        checkedRest = rest;
        const Type *element = restType->isArray() ? restType->inner() : restType;
        // A tuple of the arguments for a list of parameters.
        if (listRest && listRest->isTuple() && i - fixed < listRest->fields().size()) {
          element = listRest->fields()[i - fixed].type;
        }
        if (!call->spreads(i)) {
          arg = sema_.typeRelations().coerce(arg, element);
        } else if (arg->type != restType && !arg->type->isError() && !restType->isError()) {
          sema_.diags_.error(arg->loc(),
                       "cannot spread '" + arg->type->name() + "' into '" + restType->name() + "'");
        }
      }
      rest->elements.push_back({arg, call->spreads(i)});
    }
    if (checkedRest && listRest && listRest->isTuple() &&
        rest->elements.size() != listRest->fields().size()) {
      sema_.diags_.error(rest->loc(), "expected " + std::to_string(fixed + listRest->fields().size()) +
                                    " arguments, but got " + std::to_string(call->args.size()));
      rest->type = Type::error();
    } else if (checkedRest) {
      rest->type = restType;
    }
    call->args.resize(std::min(call->args.size(), fixed));
    while (call->args.size() < fixed) { // optional parameters left out
      call->args.push_back(sema_.ctx_.make<UndefinedExpr>(call->loc()));
    }
    call->args.push_back(rest);
    call->spread.clear();
  }
  // Parameters left out get undefined, and with it their default values.
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (call->args[i] == checkedRest) {
      continue;
    }
    call->args[i] =
        isChecked(i) ? sema_.typeRelations().coerce(call->args[i], params[i]) : checkArg(call->args[i], params[i]);
  }
  sema_.flowChecker().calledFunction();

  return call;
}

bool ArgumentChecker::checkArity(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max) {
  size_t count = call->args.size();
  if (count >= min && count <= max) {
    return true;
  }
  if (call->tagged) { // the arguments are not written out
    sema_.diags_.error(loc, "expected " + argumentCount(min, max) + ", but the tagged template passes " +
                          llvm::Twine(count) + ": the array of its texts" +
                          (count == 1   ? ""
                           : count == 2 ? " and 1 value"
                                        : " and " + std::to_string(count - 1) + " values"));
  } else {
    sema_.diags_.error(loc, "expected " + argumentCount(min, max) + ", but got " + llvm::Twine(count));
  }
  for (Expr *&arg : call->args) {
    arg = sema_.exprChecker().checkExpr(arg);
  }

  return false;
}

Expr *ArgumentChecker::checkArg(Expr *arg, const Type *to) {
  return sema_.typeRelations().coerce(sema_.exprChecker().checkExpr(arg, to->isError() ? nullptr : to), to);
}

} // namespace tz
