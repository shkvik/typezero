#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr unsigned kAnyCount = ~0u;

}

ArgumentChecker::ArgumentChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeRelations = &all.typeRelations;
  this->argListChecker = &all.argListChecker;
}

ArgumentChecker::ResultSpreadRestFits ArgumentChecker::checkRestType(const Expr *arg,
                                                                     const Type *to) const {
  if (arg->type != to && !arg->type->isError() && !to->isError()) {
    auto message = ErrorTextSpreadRest::otherType(arg->type->name(), to->name());

    return err(ArgumentCheckerSpreadRestError{
        .code = ArgumentCheckerSpreadRestErrorCode::OTHER_TYPE,
        .message = message,
    });
  }

  return ok(arg);
}

ArgumentChecker::ResultTupleRestCount ArgumentChecker::checkTupleRestCount(const ArrayLitExpr *rest,
                                                                           const Type *listRest,
                                                                           size_t fixed,
                                                                           size_t count) const {
  bool isTuple = listRest != nullptr && listRest->isTuple();
  if (isTuple && rest->elements.size() != listRest->fields().size()) {
    auto message = ErrorTextTupleRest::count(fixed + listRest->fields().size(), count);

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
  size_t restCount = hasRest ? 1 : 0;
  auto shape = ArgumentShape{
      .params = params,
      .fixed = params.size() - restCount,
      .loc = loc,
      .checked = checked,
  };
  auto least = static_cast<unsigned>(minArgs);
  bool isListParam =
      hasRest && (params.back()->isListParam() || ArgListChecker::isSpreadingLists(params.back()));
  if (isListParam) {
    return this->checkListParamArguments(call, shape, least);
  }
  size_t spreadLimit = hasRest ? shape.fixed : call->args.size();
  if (this->rejectsSpreadBefore(call, spreadLimit)) {
    return call;
  }
  unsigned most = hasRest ? kAnyCount : static_cast<unsigned>(params.size());
  auto arity = this->checkArity(call, least, most);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *checkedRest = hasRest ? this->packRestArgs(call, shape) : nullptr;
  for (size_t index = 0; index < call->args.size(); ++index) {
    if (call->args[index] != checkedRest) {
      call->args[index] = this->typedArgument(shape, call->args[index], index);
    }
  }
  this->flowChecker->calledFunction();

  return call;
}

Expr *ArgumentChecker::typedArgument(const ArgumentShape &shape, Expr *arg, size_t index) {
  if (this->isChecked(shape, index)) {
    return this->typeRelations->coerce(arg, shape.params[index]);
  }

  return this->checkArg(arg, shape.params[index]);
}

bool ArgumentChecker::isChecked(const ArgumentShape &shape, size_t index) const {
  return shape.checked != nullptr && index < shape.checked->size() && (*shape.checked)[index];
}

bool ArgumentChecker::rejectsSpreadBefore(CallExpr *call, size_t limit) {
  for (size_t index = 0; index < call->args.size() && index < limit; ++index) {
    if (call->spreads(index) && this->callChecker->rejectSpread(call)) {
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
  auto arity = this->checkArity(call, least, kAnyCount);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(shape.loc, arity.error().message);
    return call;
  }
  for (size_t index = 0; index < shape.fixed; ++index) {
    call->args[index] = this->typedArgument(shape, call->args[index], index);
  }
  this->argListChecker->checkListArguments(call, shape.fixed, shape.params.back(), shape.loc,
                                           shape.checked);

  return call;
}

Expr *ArgumentChecker::packRestArgs(CallExpr *call, const ArgumentShape &shape) {
  const Type *last = shape.params.back();
  const Type *listRest = last->isTuple() || last->isListParam() ? last : nullptr;
  bool isSpreadOnly =
      listRest != nullptr && call->args.size() == shape.fixed + 1 && call->spreads(shape.fixed);
  if (isSpreadOnly) {
    return this->checkSpreadRest(call, shape, listRest);
  }

  return this->packRestElements(call, shape, listRest);
}

Expr *ArgumentChecker::checkSpreadRest(CallExpr *call, const ArgumentShape &shape,
                                       const Type *listRest) {
  size_t fixed = shape.fixed;
  if (!this->isChecked(shape, fixed)) {
    call->args[fixed] = this->exprChecker->checkExpr(call->args[fixed], listRest);
  }
  Expr *arg = call->args[fixed];
  auto fits = this->checkRestType(arg, listRest);
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
  bool hasChecked = false;
  for (size_t index = fixed; index < call->args.size(); ++index) {
    bool isChecked = this->isChecked(shape, index);
    Expr *arg =
        isChecked ? this->checkedRestElement(call, index, shape, listRest) : call->args[index];
    hasChecked = hasChecked || isChecked;
    rest->elements.push_back(ArrayLitExpr::Element{.expr = arg, .spread = call->spreads(index)});
  }
  auto counted = this->checkTupleRestCount(rest, listRest, fixed, call->args.size());
  if (hasChecked && counted.isErr()) {
    this->state.getDiagnostics().error(rest->loc(), counted.error().message);
    rest->type = Type::error();
  } else if (hasChecked) {
    rest->type = shape.params.back();
  }
  this->replaceRestArgs(call, fixed, rest);

  return hasChecked ? rest : nullptr;
}

void ArgumentChecker::replaceRestArgs(CallExpr *call, size_t fixed, ArrayLitExpr *rest) {
  call->args.resize(std::min(call->args.size(), fixed));
  while (call->args.size() < fixed) {
    call->args.push_back(this->state.getAst().make<UndefinedExpr>(call->loc()));
  }
  call->args.push_back(rest);
  call->spread.clear();
}

Expr *ArgumentChecker::checkedRestElement(CallExpr *call, size_t index, const ArgumentShape &shape,
                                          const Type *listRest) {
  Expr *arg = call->args[index];
  if (!call->spreads(index)) {
    const auto *element = this->restElementType(index, shape, listRest);

    return this->typeRelations->coerce(arg, element);
  }
  auto fits = this->checkRestType(arg, shape.params.back());
  if (fits.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), fits.error().message);
  }

  return arg;
}

const Type *ArgumentChecker::restElementType(size_t index, const ArgumentShape &shape,
                                             const Type *listRest) const {
  const Type *restType = shape.params.back();
  size_t at = index - shape.fixed;
  bool isTupleSlot = listRest != nullptr && listRest->isTuple() && at < listRest->fields().size();
  if (isTupleSlot) {
    return listRest->fields()[at].type;
  }

  return restType->isArray() ? restType->inner() : restType;
}

void ArgumentChecker::walkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
}

ArgumentChecker::ResultArityMatches ArgumentChecker::checkArity(CallExpr *call, unsigned min,
                                                                unsigned max) {
  size_t count = call->args.size();
  bool isFitting = count >= min && count <= max;
  if (!isFitting && call->tagged) {
    this->walkArgsLoosely(call);
    auto message = ErrorTextArity::tagged(min, max, count);

    return err(ArgumentCheckerCheckArityError{
        .code = ArgumentCheckerCheckArityErrorCode::WRONG_COUNT_TAGGED,
        .message = message,
    });
  }
  if (!isFitting) {
    this->walkArgsLoosely(call);
    auto message = ErrorTextArity::count(min, max, count);

    return err(ArgumentCheckerCheckArityError{
        .code = ArgumentCheckerCheckArityErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(call);
}

Expr *ArgumentChecker::checkArg(Expr *arg, const Type *to) {
  const Type *expected = to->isError() ? nullptr : to;
  auto *checked = this->exprChecker->checkExpr(arg, expected);

  return this->typeRelations->coerce(checked, to);
}

Expr *ArgumentChecker::checkAnyNumber(Expr *arg) {
  auto *checked = this->exprChecker->checkExpr(arg);
  if (checked->type->isInteger()) {
    return this->typeRelations->convert(checked, Type::doubleTy());
  }

  return this->typeRelations->coerce(checked, Type::doubleTy());
}

}
