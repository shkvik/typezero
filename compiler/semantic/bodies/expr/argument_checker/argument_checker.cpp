#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ArgumentChecker::ArgumentChecker(SemanticState &owner)
    : state(owner) {}

ArgumentChecker::ResultSpreadRestFits ArgumentChecker::spreadRest(const Expr *arg, const Type *to) {
  if (arg->type != to && !arg->type->isError() && !to->isError()) {
    std::string message = ErrorTextSpreadRest::otherType(arg->type->name(), to->name());

    return err(ArgumentCheckerSpreadRestError{
        .code = ArgumentCheckerSpreadRestErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(arg);
}

ArgumentChecker::ResultTupleRestCount ArgumentChecker::tupleRest(const ArrayLitExpr *rest,
                                                                 const Type *listRest, size_t fixed,
                                                                 size_t count) {
  bool tuple = listRest && listRest->isTuple();
  if (tuple && rest->elements.size() != listRest->fields().size()) {
    std::string message = ErrorTextTupleRest::count(fixed + listRest->fields().size(), count);

    return err(ArgumentCheckerTupleRestError{
        .code = ArgumentCheckerTupleRestErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(rest);
}

Expr *ArgumentChecker::checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params,
                                      size_t minArgs, bool hasRest, llvm::SMLoc loc,
                                      const std::vector<bool> *checked) {
  if (!checked) {
    ArgListChecker(this->state).expandListSpreads(call->args, call->spread);
  }
  size_t fixed = params.size() - (hasRest ? 1 : 0);
  unsigned least = static_cast<unsigned>(minArgs);
  bool listParam =
      hasRest && (params.back()->isListParam() || ArgListChecker::spreadsLists(params.back()));
  if (listParam) {
    for (size_t i = 0; i < call->args.size() && i < fixed; ++i) {
      if (call->spreads(i) && CallChecker(this->state).rejectSpread(call)) {
        return call;
      }
    }
    ResultArityMatches arity = this->checkArity(call, least, ~0u);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    for (size_t i = 0; i < fixed; ++i) {
      bool given = checked && i < checked->size() && (*checked)[i];
      call->args[i] = given ? TypeRelations(this->state).coerce(call->args[i], params[i])
                            : this->checkArg(call->args[i], params[i]);
    }
    ArgListChecker(this->state).checkListArguments(call, fixed, params.back(), loc, checked);

    return call;
  }
  size_t spreadLimit = hasRest ? fixed : call->args.size();
  for (size_t i = 0; i < call->args.size() && i < spreadLimit; ++i) {
    if (call->spreads(i) && CallChecker(this->state).rejectSpread(call)) {
      return call;
    }
  }
  unsigned most = hasRest ? ~0u : static_cast<unsigned>(params.size());
  ResultArityMatches arity = this->checkArity(call, least, most);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  auto isChecked = [&](size_t i) { return checked && i < checked->size() && (*checked)[i]; };
  Expr *checkedRest = nullptr;
  bool listRestParam = hasRest && (params.back()->isTuple() || params.back()->isListParam());
  const Type *listRest = listRestParam ? params.back() : nullptr;
  if (listRest && call->args.size() == fixed + 1 && call->spreads(fixed)) {
    Expr *&arg = call->args[fixed];
    if (!isChecked(fixed)) {
      arg = ExprChecker(this->state).checkExpr(arg, listRest);
    }
    ResultSpreadRestFits fits = ArgumentChecker::spreadRest(arg, listRest);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), fits.error().message);
    }
    checkedRest = arg;
    call->spread.clear();
  } else if (hasRest) {
    llvm::SMLoc restLoc = call->args.size() > fixed ? call->args[fixed]->loc() : call->loc();
    auto *rest = this->state.getAst().make<ArrayLitExpr>(restLoc);
    const Type *restType = params.back();
    for (size_t i = fixed; i < call->args.size(); ++i) {
      Expr *arg = call->args[i];
      if (isChecked(i)) {
        checkedRest = rest;
        const Type *element = restType->isArray() ? restType->inner() : restType;
        if (listRest && listRest->isTuple() && i - fixed < listRest->fields().size()) {
          element = listRest->fields()[i - fixed].type;
        }
        if (!call->spreads(i)) {
          arg = TypeRelations(this->state).coerce(arg, element);
        } else {
          ResultSpreadRestFits fits = ArgumentChecker::spreadRest(arg, restType);
          if (fits.isErr()) {
            this->state.getDiagnostics().error(arg->loc(), fits.error().message);
          }
        }
      }
      rest->elements.push_back({arg, call->spreads(i)});
    }
    ResultTupleRestCount counted =
        ArgumentChecker::tupleRest(rest, listRest, fixed, call->args.size());
    if (checkedRest && counted.isErr()) {
      this->state.getDiagnostics().error(rest->loc(), counted.error().message);
      rest->type = Type::error();
    } else if (checkedRest) {
      rest->type = restType;
    }
    call->args.resize(std::min(call->args.size(), fixed));
    while (call->args.size() < fixed) {
      call->args.push_back(this->state.getAst().make<UndefinedExpr>(call->loc()));
    }
    call->args.push_back(rest);
    call->spread.clear();
  }
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (call->args[i] == checkedRest) {
      continue;
    }
    call->args[i] = isChecked(i) ? TypeRelations(this->state).coerce(call->args[i], params[i])
                                 : this->checkArg(call->args[i], params[i]);
  }
  FlowChecker(this->state).calledFunction();

  return call;
}

void ArgumentChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }
}

ArgumentChecker::ResultArityMatches ArgumentChecker::checkArity(CallExpr *call, unsigned min,
                                                                unsigned max) {
  size_t count = call->args.size();
  bool fits = count >= min && count <= max;
  if (!fits && call->tagged) {
    this->checkArgsLoosely(call);
    std::string message = ErrorTextArity::tagged(min, max, count);

    return err(ArgumentCheckerCheckArityError{
        .code = ArgumentCheckerCheckArityErrorCode::WRONG_COUNT_TAGGED,
        .message = message,
    });
  }
  if (!fits) {
    this->checkArgsLoosely(call);
    std::string message = ErrorTextArity::count(min, max, count);

    return err(ArgumentCheckerCheckArityError{
        .code = ArgumentCheckerCheckArityErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(call);
}

Expr *ArgumentChecker::checkArg(Expr *arg, const Type *to) {
  const Type *expected = to->isError() ? nullptr : to;
  Expr *checked = ExprChecker(this->state).checkExpr(arg, expected);

  return TypeRelations(this->state).coerce(checked, to);
}

Expr *ArgumentChecker::checkAnyNumber(Expr *arg) {
  arg = ExprChecker(this->state).checkExpr(arg);
  if (arg->type->isInteger()) {
    return TypeRelations(this->state).convert(arg, Type::doubleTy());
  }

  return TypeRelations(this->state).coerce(arg, Type::doubleTy());
}

}
