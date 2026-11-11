#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ArgumentChecker::ArgumentChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeRelations = &all.typeRelations;
  this->argListChecker = &all.argListChecker;
}

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
  if (checked == nullptr) {
    this->argListChecker->expandListSpreads(call->args, call->spread);
  }
  ArgumentShape shape{
      .params = params,
      .fixed = params.size() - (hasRest ? 1 : 0),
      .loc = loc,
      .checked = checked,
  };
  auto least = static_cast<unsigned>(minArgs);
  bool listParam =
      hasRest && (params.back()->isListParam() || ArgListChecker::spreadsLists(params.back()));
  if (listParam) {
    return this->checkListParamArguments(call, shape, least);
  }
  size_t spreadLimit = hasRest ? shape.fixed : call->args.size();
  if (this->rejectsSpreadBefore(call, spreadLimit)) {
    return call;
  }
  unsigned most = hasRest ? ~0u : static_cast<unsigned>(params.size());
  ResultArityMatches arity = this->checkArity(call, least, most);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *checkedRest = hasRest ? this->packRestArgs(call, shape) : nullptr;
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (call->args[i] == checkedRest) {
      continue;
    }
    call->args[i] = ArgumentChecker::isChecked(shape, i)
                        ? this->typeRelations->coerce(call->args[i], params[i])
                        : this->checkArg(call->args[i], params[i]);
  }
  this->flowChecker->calledFunction();

  return call;
}

bool ArgumentChecker::isChecked(const ArgumentShape &shape, size_t i) {
  return shape.checked != nullptr && i < shape.checked->size() && (*shape.checked)[i];
}

bool ArgumentChecker::rejectsSpreadBefore(CallExpr *call, size_t limit) {
  for (size_t i = 0; i < call->args.size() && i < limit; ++i) {
    if (call->spreads(i) && this->callChecker->rejectSpread(call)) {
      return true;
    }
  }

  return false;
}

Expr *ArgumentChecker::checkListParamArguments(CallExpr *call, const ArgumentShape &shape,
                                               unsigned least) {
  if (this->rejectsSpreadBefore(call, shape.fixed)) {
    return call;
  }
  ResultArityMatches arity = this->checkArity(call, least, ~0u);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(shape.loc, arity.error().message);
    return call;
  }
  for (size_t i = 0; i < shape.fixed; ++i) {
    call->args[i] = ArgumentChecker::isChecked(shape, i)
                        ? this->typeRelations->coerce(call->args[i], shape.params[i])
                        : this->checkArg(call->args[i], shape.params[i]);
  }
  this->argListChecker->checkListArguments(call, shape.fixed, shape.params.back(), shape.loc,
                                           shape.checked);

  return call;
}

Expr *ArgumentChecker::packRestArgs(CallExpr *call, const ArgumentShape &shape) {
  const Type *last = shape.params.back();
  const Type *listRest = last->isTuple() || last->isListParam() ? last : nullptr;
  bool spreadOnly =
      listRest != nullptr && call->args.size() == shape.fixed + 1 && call->spreads(shape.fixed);
  if (spreadOnly) {
    return this->checkSpreadRest(call, shape, listRest);
  }

  return this->packRestElements(call, shape, listRest);
}

Expr *ArgumentChecker::checkSpreadRest(CallExpr *call, const ArgumentShape &shape,
                                       const Type *listRest) {
  size_t fixed = shape.fixed;
  if (!ArgumentChecker::isChecked(shape, fixed)) {
    call->args[fixed] = this->exprChecker->checkExpr(call->args[fixed], listRest);
  }
  Expr *arg = call->args[fixed];
  ResultSpreadRestFits fits = ArgumentChecker::spreadRest(arg, listRest);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fits.error().message);
  }
  call->spread.clear();

  return arg;
}

Expr *ArgumentChecker::packRestElements(CallExpr *call, const ArgumentShape &shape,
                                        const Type *listRest) {
  size_t fixed = shape.fixed;
  llvm::SMLoc restLoc = call->args.size() > fixed ? call->args[fixed]->loc() : call->loc();
  auto *rest = this->state.getAst().make<ArrayLitExpr>(restLoc);
  const Type *restType = shape.params.back();
  Expr *checkedRest = nullptr;
  for (size_t i = fixed; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    if (ArgumentChecker::isChecked(shape, i)) {
      checkedRest = rest;
      arg = this->checkedRestElement(call, i, shape, listRest);
    }
    rest->elements.push_back({arg, call->spreads(i)});
  }
  ResultTupleRestCount counted =
      ArgumentChecker::tupleRest(rest, listRest, fixed, call->args.size());
  if (checkedRest != nullptr && counted.isErr()) {
    this->state.getDiagnostics().error(rest->loc(), counted.error().message);
    rest->type = Type::error();
  } else if (checkedRest != nullptr) {
    rest->type = restType;
  }
  call->args.resize(std::min(call->args.size(), fixed));
  while (call->args.size() < fixed) {
    call->args.push_back(this->state.getAst().make<UndefinedExpr>(call->loc()));
  }
  call->args.push_back(rest);
  call->spread.clear();

  return checkedRest;
}

Expr *ArgumentChecker::checkedRestElement(CallExpr *call, size_t i, const ArgumentShape &shape,
                                          const Type *listRest) {
  Expr *arg = call->args[i];
  const Type *restType = shape.params.back();
  const Type *element = restType->isArray() ? restType->inner() : restType;
  size_t at = i - shape.fixed;
  if (listRest != nullptr && listRest->isTuple() && at < listRest->fields().size()) {
    element = listRest->fields()[at].type;
  }
  if (!call->spreads(i)) {
    return this->typeRelations->coerce(arg, element);
  }
  ResultSpreadRestFits fits = ArgumentChecker::spreadRest(arg, restType);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fits.error().message);
  }

  return arg;
}

void ArgumentChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
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
  Expr *checked = this->exprChecker->checkExpr(arg, expected);

  return this->typeRelations->coerce(checked, to);
}

Expr *ArgumentChecker::checkAnyNumber(Expr *arg) {
  arg = this->exprChecker->checkExpr(arg);
  if (arg->type->isInteger()) {
    return this->typeRelations->convert(arg, Type::doubleTy());
  }

  return this->typeRelations->coerce(arg, Type::doubleTy());
}

}
