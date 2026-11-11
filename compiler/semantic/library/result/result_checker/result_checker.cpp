#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ResultChecker::ResultChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->resultTypes = &all.resultTypes;
  this->asyncChecker = &all.asyncChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->argumentChecker = &all.argumentChecker;
}

const Type *ResultChecker::halfParam(const Type *half) {
  return half->isVoid() ? Type::undefinedTy() : half;
}

bool ResultChecker::givesType(const Type *half) {
  return !half->isNever() && !half->isVoid();
}

std::string ResultChecker::receiverText(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && !name->name.starts_with("%") ? name->name.str() : "r";
}

const Expr *ResultChecker::unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

Expr *ResultChecker::checkResultCall(CallExpr *call, NameExpr *name, const Type *expected) {
  std::optional<GlobalName> which = this->resultTypes->resultName(name->name);
  if (which != GlobalName::OK && which != GlobalName::ERR) {
    return nullptr;
  }

  return this->checkOkErr(call, name, which == GlobalName::OK, expected);
}

ResultChecker::ResultOkErrTypeArgs ResultChecker::okErrTypeArgs(const NameExpr *name) {
  if (!name->typeArgs.empty()) {
    std::string message = ErrorTextOkErr::typeArgs(name->name);

    return err(ResultCheckerOkErrTypeArgsError{
        .code = ResultCheckerOkErrTypeArgsErrorCode::NOT_TWO_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(name);
}

// TODO: говнокод
Expr *ResultChecker::checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected) {
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(name->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }

  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && !wanted->isResult()) {
    wanted = nullptr;
  }
  if (name->typeArgs.size() == 2) {
    const Type *value = this->typeResolver->resolveType(name->typeArgs[0], true);
    const Type *error = this->typeResolver->resolveType(name->typeArgs[1], true);
    if (value->isError() || error->isError()) {
      this->checkArgsLoosely(call);
      call->type = Type::error();
      return call;
    }
    wanted = Type::result(value, error);
  } else {
    ResultOkErrTypeArgs typeArgs = ResultChecker::okErrTypeArgs(name);
    if (typeArgs.isErr()) {
      this->state.getDiagnostics().error(name->loc(), typeArgs.error().message);
      return this->markCallFailed(call);
    }
  }
  const Type *wantedHalf = wanted ? (isOk ? wanted->okType() : wanted->errType()) : nullptr;
  const Type *half = Type::voidTy();
  if (!call->args.empty()) {
    Expr *&arg = call->args[0];
    bool hinted = wantedHalf && ResultChecker::givesType(wantedHalf);
    arg = this->exprChecker->checkExpr(arg, hinted ? wantedHalf : nullptr);
    half = arg->type;
    if (half->isVoid()) {
      std::string message = ErrorTextOkErr::voidValue(name->name);
      this->state.getDiagnostics().error(arg->loc(), message);
      half = Type::error();
    } else if (hinted && TypeRelations::isImplicitlyConvertible(half, wantedHalf)) {
      arg = this->typeRelations->convert(arg, wantedHalf);
      half = wantedHalf;
    } else if (name->typeArgs.size() == 2 && !half->isError()) {
      arg = this->typeRelations->coerce(arg, wantedHalf);
      half = wantedHalf;
    }
  }
  const Type *other = wanted ? (isOk ? wanted->errType() : wanted->okType()) : Type::never();
  call->target = BuiltinCall{.builtin = isOk ? Builtin::ResultOk : Builtin::ResultErr};
  call->type = isOk ? Type::result(half, other) : Type::result(other, half);

  return call;
}

void ResultChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    auto *fe = dyn_cast<FunctionExpr>(arg);
    if (!fe) {
      arg = this->exprChecker->checkExpr(arg);
      continue;
    }
    Signature loose;
    loose.params.assign(fe->fn->params.size(), Type::error());
    loose.pad = false;
    arg = this->closureChecker->checkFunctionExpr(fe, &loose);
  }
}

Expr *ResultChecker::markCallFailed(CallExpr *call) {
  this->checkArgsLoosely(call);
  call->type = Type::error();

  return call;
}

ResultChecker::ResultStaticIsKnown ResultChecker::staticMethod(const NameExpr *name,
                                                               llvm::StringRef method) {
  if (method != "combine" && method != "combineWithAllErrors") {
    std::string message = ErrorTextCombine::noStatic(name->name, method);

    return err(ResultCheckerStaticMethodError{
        .code = ResultCheckerStaticMethodErrorCode::NO_SUCH_STATIC_METHOD,
        .message = message,
    });
  }

  return ok(method);
}

Expr *ResultChecker::checkResultStatic(CallExpr *call, MemberExpr *member) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (name == nullptr || call->optional || member->optional) {
    return nullptr;
  }
  std::optional<GlobalName> which = this->resultTypes->resultName(name->name);
  if (which != GlobalName::RESULT) {
    return nullptr;
  }
  llvm::StringRef method = member->name;
  ResultStaticIsKnown staticMethod = ResultChecker::staticMethod(name, method);
  if (staticMethod.isErr()) {
    this->state.getDiagnostics().error(member->loc(), staticMethod.error().message);
    return this->markCallFailed(call);
  }
  if (!this->combineArityFits(call, member)) {
    return call;
  }
  bool all = method == "combineWithAllErrors";
  call->target =
      BuiltinCall{.builtin = all ? Builtin::ResultCombineWithAllErrors : Builtin::ResultCombine};
  CombineCall combine{
      .call = call,
      .what = name->name.str() + "." + method.str(),
      .all = all,
  };
  auto *lit = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (lit != nullptr && ResultChecker::isPlainList(lit)) {
    return this->checkCombineList(combine, lit);
  }

  return this->checkCombineArray(combine);
}

bool ResultChecker::combineArityFits(CallExpr *call, const MemberExpr *member) {
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }

  return !rejected && arity.isOk();
}

bool ResultChecker::isPlainList(const ArrayLitExpr *lit) {
  if (lit->elements.empty()) {
    return false;
  }
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (e.spread || e.hole) {
      return false;
    }
  }

  return true;
}

bool ResultChecker::holdsValues(const CombineCall &combine, const Type *result, const Expr *at) {
  bool values = ResultChecker::givesType(result->okType());
  if (values && !(combine.all && result->errType()->isVoid())) {
    return true;
  }
  std::string message = ErrorTextCombine::holds(combine.what, values, result->name());
  this->state.getDiagnostics().error(at->loc(), message);

  return false;
}

const Type *ResultChecker::errorsOf(const CombineCall &combine, const Type *error) {
  return !combine.all || error->isNever() ? error : Type::array(error);
}

Expr *ResultChecker::checkCombineList(const CombineCall &combine, ArrayLitExpr *lit) {
  CallExpr *call = combine.call;
  std::vector<const Type *> results;
  for (ArrayLitExpr::Element &e : lit->elements) {
    e.expr = this->exprChecker->checkExpr(e.expr);
    results.push_back(e.expr->type);
  }
  for (const Type *t : results) {
    if (t->isError()) {
      return call;
    }
  }
  for (size_t i = 0; i < results.size(); ++i) {
    if (!results[i]->isResult()) {
      std::string message = ErrorTextCombine::notResult(combine.what, results[i]->name());
      this->state.getDiagnostics().error(lit->elements[i].expr->loc(), message);
      return call;
    }
  }
  const Type *common = ResultChecker::commonResult(results);
  if (common != nullptr && !common->isError()) {
    return this->checkSameCombine(combine, lit, common);
  }

  return this->checkTupleCombine(combine, lit, results);
}

const Type *ResultChecker::sameValues(const Type *a, const Type *b) {
  const Type *x = a->okType();
  const Type *y = b->okType();
  const Type *value = x == y || y->isNever()       ? x
                      : x->isNever()               ? y
                      : x->isVoid() || y->isVoid() ? nullptr
                                                   : TypeRelations::unify(x, y);
  const Type *error = ResultTypes::unifyHalf(a->errType(), b->errType());
  bool joined = value != nullptr && !value->isError() && error != nullptr;

  return joined ? Type::result(value, error) : nullptr;
}

const Type *ResultChecker::commonResult(llvm::ArrayRef<const Type *> results) {
  const Type *common = results[0];
  for (const Type *t : results) {
    common = common != nullptr ? ResultChecker::sameValues(common, t) : nullptr;
  }

  return common;
}

Expr *ResultChecker::checkSameCombine(const CombineCall &combine, ArrayLitExpr *lit,
                                      const Type *common) {
  CallExpr *call = combine.call;
  if (!this->holdsValues(combine, common, call->args[0])) {
    return call;
  }
  for (ArrayLitExpr::Element &e : lit->elements) {
    e.expr = this->typeRelations->convert(e.expr, common);
  }
  lit->type = Type::array(common);
  const Type *values = Type::array(common->okType());
  const Type *errors = ResultChecker::errorsOf(combine, common->errType());
  call->type = Type::result(values, errors);

  return call;
}

Expr *ResultChecker::checkTupleCombine(const CombineCall &combine, ArrayLitExpr *lit,
                                       const std::vector<const Type *> &results) {
  CallExpr *call = combine.call;
  std::vector<const Type *> values;
  const Type *error = Type::never();
  for (size_t i = 0; i < results.size(); ++i) {
    if (!this->holdsValues(combine, results[i], lit->elements[i].expr)) {
      return call;
    }
    values.push_back(results[i]->okType());
    const Type *before = error;
    error = ResultTypes::unifyHalf(error, results[i]->errType());
    if (error == nullptr) {
      std::string message = ErrorTextCombine::noUnion(combine.what, before, results[i]->errType());
      this->state.getDiagnostics().error(lit->elements[i].expr->loc(), message);
      return call;
    }
  }
  lit->type = Type::tuple(results);
  const Type *tuple = Type::tuple(values);
  const Type *errors = ResultChecker::errorsOf(combine, error);
  call->type = Type::result(tuple, errors);

  return call;
}

Expr *ResultChecker::checkCombineArray(const CombineCall &combine) {
  CallExpr *call = combine.call;
  Expr *&arg = call->args[0];
  arg = this->exprChecker->checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (!type->isArray() || !type->inner()->isResult()) {
    std::string message = ErrorTextCombine::notArray(combine.what, type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return call;
  }
  const Type *element = type->inner();
  if (!this->holdsValues(combine, element, arg)) {
    return call;
  }
  const Type *values = Type::array(element->okType());
  const Type *errors = ResultChecker::errorsOf(combine, element->errType());
  call->type = Type::result(values, errors);

  return call;
}

Expr *ResultChecker::checkResultMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string r = ResultChecker::receiverText(member->object);
  auto defined = [&](MemberExpr *half) -> Expr * {
    if (Expr *narrowed = this->fieldFacts->narrowedField(half); narrowed != half) {
      return narrowed;
    }
    if (half->type->isOptional()) {
      if (std::optional<FieldPath> path = this->fieldFacts->fieldPath(half);
          path && this->fieldFacts->isFieldDefined(*path)) {
        return this->typeRelations->convert(half, half->type->inner());
      }
    }
    return half;
  };
  std::optional<BuiltinProperty> property = BuiltinPropertyTable::find(name);
  if (property == BuiltinProperty::RESULT_VALUE) {
    if (type->errType()->isNever()) {
      member->type = ResultChecker::halfParam(type->okType());
      return defined(member);
    }
    std::string message = ErrorTextResultMember::value(r);
    this->state.getDiagnostics().error(member->loc(), message);
  } else if (property == BuiltinProperty::RESULT_ERROR) {
    if (type->okType()->isNever()) {
      member->type = ResultChecker::halfParam(type->errType());
      return defined(member);
    }
    std::string message = ErrorTextResultMember::error(r);
    this->state.getDiagnostics().error(member->loc(), message);
  } else {
    const ResultMethodInfo *method = ResultMethodTable::find(name);
    std::string message = method != nullptr ? ErrorTextResultMember::method(name, r)
                                            : ErrorTextResultMember::noMember(name, type->name());
    this->state.getDiagnostics().error(member->loc(), message);
  }

  return member;
}

ResultChecker::ResultValueGetsUsed ResultChecker::usedValue(const Expr *expr, bool callback,
                                                            llvm::SMLoc at) {
  const Type *type = expr->type;
  if (type && type->holdsPromises()) {
    AsyncChecker::ResultPromiseHandled handled =
        this->asyncChecker->checkDiscardedPromise(expr, callback, at);
    if (handled.isErr()) {
      return err(ResultCheckerUsedValueError{
          .code = ResultCheckerUsedValueErrorCode::PROMISE_DROPPED,
          .message = handled.error().message,
      });
    }
    return ok(expr);
  }
  if (!type || !type->holdsResults() || isa<AssignExpr>(expr)) {
    return ok(expr);
  }
  auto *call = dyn_cast<CallExpr>(expr);
  bool taken =
      call && (call->builtin() == Builtin::ArrPop || call->builtin() == Builtin::ArrShift ||
               call->builtin() == Builtin::ArrSplice);
  if (taken) {
    return ok(expr);
  }
  if (!type->nonOptional()->isResult()) {
    bool mapped =
        call && (call->builtin() == Builtin::ArrMap || call->builtin() == Builtin::ArrFlatMap);
    if (mapped) {
      std::string message = ErrorTextUnusedResult::results();

      return err(ResultCheckerUsedValueError{
          .code = ResultCheckerUsedValueErrorCode::RESULTS_UNUSED,
          .message = message,
      });
    }
    return ok(expr);
  }
  if (callback) {
    std::string message = ErrorTextUnusedResult::callback();

    return err(ResultCheckerUsedValueError{
        .code = ResultCheckerUsedValueErrorCode::CALLBACK_RESULT_DROPPED,
        .message = message,
    });
  }

  std::string message = ErrorTextUnusedResult::result();

  return err(ResultCheckerUsedValueError{
      .code = ResultCheckerUsedValueErrorCode::RESULT_UNUSED,
      .message = message,
  });
}

void ResultChecker::addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (!call ||
      (call->builtin() != Builtin::ResultIsOk && call->builtin() != Builtin::ResultIsErr)) {
    return;
  }
  auto *name =
      dyn_cast<NameExpr>(ResultChecker::unconverted(cast<MemberExpr>(call->callee)->object));
  if (!name || !name->var() || !name->var()->type ||
      !name->var()->type->nonOptional()->isResult()) {
    return;
  }
  bool isOk = call->builtin() == Builtin::ResultIsOk;
  Flow &ok = isOk ? whenTrue : whenFalse;
  Flow &err = isOk ? whenFalse : whenTrue;
  ok.okResults.insert(name->var());
  ok.errResults.erase(name->var());
  err.errResults.insert(name->var());
  err.okResults.erase(name->var());
}

}
