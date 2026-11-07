#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker.h"
#include "compiler/pipeline/check_program/flow/field_facts/field_facts.h"
#include "compiler/pipeline/check_program/topics/async/async_checker/async_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/functions/closure_checker/closure_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_types/result_types.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

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

static std::string noUnion(const Type *a, const Type *b) {
  if (a->isVoid() || b->isVoid()) {
    return ": one of them is nothing, 'ok()'";
  }
  std::string problem = unionProblem(Type::unionOf({a, b}));

  return problem.empty() ? "" : ": " + problem;
}

ResultChecker::ResultChecker(CheckState &owner)
    : state(owner) {}

Expr *ResultChecker::checkResultCall(CallExpr *call, NameExpr *name, const Type *expected) {
  llvm::StringRef which = ResultTypes(this->state).resultName(name->name);
  if (which != "ok" && which != "err") {
    return nullptr;
  }

  return this->checkOkErr(call, name, which == "ok", expected);
}

Expr *ResultChecker::checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected) {
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 0, 1);
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
    const Type *value = TypeResolver(this->state).resolveType(name->typeArgs[0], true);
    const Type *error = TypeResolver(this->state).resolveType(name->typeArgs[1], true);
    if (value->isError() || error->isError()) {
      this->checkArgsLoosely(call);
      call->type = Type::error();
      return call;
    }
    wanted = Type::result(value, error);
  } else if (!name->typeArgs.empty()) {
    return this->reject(call, name->loc(),
                        "'" + name->name.str() + "' takes two type arguments: '" +
                            name->name.str() + "<T, E>(...)'");
  }
  const Type *wantedHalf = wanted ? (isOk ? wanted->okType() : wanted->errType()) : nullptr;
  const Type *half = Type::voidTy();
  if (!call->args.empty()) {
    Expr *&arg = call->args[0];
    bool hinted = wantedHalf && ResultChecker::givesType(wantedHalf);
    arg = ExprChecker(this->state).checkExpr(arg, hinted ? wantedHalf : nullptr);
    half = arg->type;
    if (half->isVoid()) {
      this->state.getDiagnostics().error(
          arg->loc(),
          "an expression of type 'void' has no value to keep; write '" + name->name + "()'");
      half = Type::error();
    } else if (hinted && TypeRelations::isImplicitlyConvertible(half, wantedHalf)) {
      arg = TypeRelations(this->state).convert(arg, wantedHalf);
      half = wantedHalf;
    } else if (name->typeArgs.size() == 2 && !half->isError()) {
      arg = TypeRelations(this->state).coerce(arg, wantedHalf);
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
      arg = ExprChecker(this->state).checkExpr(arg);
      continue;
    }
    Signature loose;
    loose.params.assign(fe->fn->params.size(), Type::error());
    loose.pad = false;
    arg = ClosureChecker(this->state).checkFunctionExpr(fe, &loose);
  }
}

Expr *ResultChecker::reject(CallExpr *call, llvm::SMLoc loc, const std::string &why) {
  this->state.getDiagnostics().error(loc, why);
  this->checkArgsLoosely(call);
  call->type = Type::error();

  return call;
}

Expr *ResultChecker::checkResultStatic(CallExpr *call, MemberExpr *member) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (!name || call->optional || member->optional ||
      ResultTypes(this->state).resultName(name->name) != "Result") {
    return nullptr;
  }
  llvm::StringRef method = member->name;
  if (method != "combine" && method != "combineWithAllErrors") {
    return this->reject(call, member->loc(),
                        "'" + name->name.str() + "." + method.str() +
                            "' does not exist; Result has 'combine' and "
                            "'combineWithAllErrors'");
  }
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  bool all = method == "combineWithAllErrors";
  call->target =
      BuiltinCall{.builtin = all ? Builtin::ResultCombineWithAllErrors : Builtin::ResultCombine};
  std::string what = name->name.str() + "." + method.str();
  auto holds = [&](const Type *result, const Expr *at) {
    if (ResultChecker::givesType(result->okType()) && !(all && result->errType()->isVoid())) {
      return true;
    }
    this->state.getDiagnostics().error(
        at->loc(), "'" + what + "' needs Results that hold " +
                       (ResultChecker::givesType(result->okType()) ? "errors" : "values") +
                       ", found '" + result->name() +
                       "'; check them one by one, e.g. with 'every(r => r.isOk())'");

    return false;
  };
  auto errorsOf = [&](const Type *error) {
    return !all || error->isNever() ? error : Type::array(error);
  };

  Expr *&arg = call->args[0];
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg);
      lit && !lit->elements.empty() &&
      llvm::none_of(lit->elements,
                    [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; })) {
    std::vector<const Type *> results;
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = ExprChecker(this->state).checkExpr(e.expr);
      results.push_back(e.expr->type);
    }
    if (llvm::any_of(results, [](const Type *t) { return t->isError(); })) {
      return call;
    }
    auto other = llvm::find_if(results, [](const Type *t) { return !t->isResult(); });
    if (other != results.end()) {
      this->state.getDiagnostics().error(lit->elements[other - results.begin()].expr->loc(),
                                         "'" + what + "' takes Results, found '" +
                                             (*other)->name() + "'");
      return call;
    }
    auto sameValues = [](const Type *a, const Type *b) -> const Type * {
      const Type *x = a->okType();
      const Type *y = b->okType();
      const Type *value = x == y || y->isNever()       ? x
                          : x->isNever()               ? y
                          : x->isVoid() || y->isVoid() ? nullptr
                                                       : TypeRelations::unify(x, y);
      const Type *error = ResultTypes::unifyHalf(a->errType(), b->errType());

      return value && !value->isError() && error ? Type::result(value, error) : nullptr;
    };
    const Type *common = results[0];
    for (const Type *t : results) {
      common = common ? sameValues(common, t) : nullptr;
    }
    if (common && !common->isError()) {
      if (!holds(common, arg)) {
        return call;
      }
      for (ArrayLitExpr::Element &e : lit->elements) {
        e.expr = TypeRelations(this->state).convert(e.expr, common);
      }
      lit->type = Type::array(common);
      call->type = Type::result(Type::array(common->okType()), errorsOf(common->errType()));
      return call;
    }
    std::vector<const Type *> values;
    const Type *error = Type::never();
    for (size_t i = 0; i < results.size(); ++i) {
      if (!holds(results[i], lit->elements[i].expr)) {
        return call;
      }
      values.push_back(results[i]->okType());
      const Type *before = error;
      if (!(error = ResultTypes::unifyHalf(error, results[i]->errType()))) {
        this->state.getDiagnostics().error(
            lit->elements[i].expr->loc(),
            "the Results given to '" + what + "' have errors that no union holds" +
                noUnion(before, results[i]->errType()) + "; convert them with 'mapErr'");

        return call;
      }
    }
    lit->type = Type::tuple(results);
    call->type = Type::result(Type::tuple(values), errorsOf(error));
    return call;
  }

  arg = ExprChecker(this->state).checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (!type->isArray() || !type->inner()->isResult()) {
    this->state.getDiagnostics().error(
        arg->loc(), "'" + what + "' takes an array of Results, found '" + type->name() + "'");
    return call;
  }
  const Type *element = type->inner();
  if (!holds(element, arg)) {
    return call;
  }
  call->type = Type::result(Type::array(element->okType()), errorsOf(element->errType()));

  return call;
}

Expr *ResultChecker::checkResultMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string r = ResultChecker::receiverText(member->object);
  auto defined = [&](MemberExpr *half) -> Expr * {
    if (Expr *narrowed = FieldFacts(this->state).narrowedField(half); narrowed != half) {
      return narrowed;
    }
    if (half->type->isOptional()) {
      if (std::optional<FieldPath> path = FieldFacts(this->state).fieldPath(half);
          path && FieldFacts(this->state).isFieldDefined(*path)) {
        return TypeRelations(this->state).convert(half, half->type->inner());
      }
    }
    return half;
  };
  if (name == "value") {
    if (type->errType()->isNever()) {
      member->type = ResultChecker::halfParam(type->okType());
      return defined(member);
    }
    this->state.getDiagnostics().error(
        member->loc(), "'value' is there only when the Result is an Ok: check it first, 'if (" + r +
                           ".isOk())', or use 'match' or 'unwrapOr'");
  } else if (name == "error") {
    if (type->okType()->isNever()) {
      member->type = ResultChecker::halfParam(type->errType());
      return defined(member);
    }
    this->state.getDiagnostics().error(
        member->loc(), "'error' is there only when the Result is an Err: check it first, 'if (" +
                           r + ".isErr())', or use 'match'");
  } else {
    static const char *const methods[] = {"isOk",    "isErr",  "map",       "mapErr",
                                          "andThen", "orElse", "match",     "unwrapOr",
                                          "andTee",  "orTee",  "andThrough"};
    if (llvm::is_contained(methods, name)) {
      this->state.getDiagnostics().error(member->loc(), "'" + name +
                                                            "' is a method of Result; call it: '" +
                                                            r + "." + name + "(...)'");
    } else {
      this->state.getDiagnostics().error(
          member->loc(), "property '" + name + "' does not exist on type '" + type->name() + "'");
    }
  }

  return member;
}

void ResultChecker::checkDiscarded(const Expr *expr, bool callback, llvm::SMLoc at) {
  const Type *type = expr->type;
  if (type && type->holdsPromises()) {
    AsyncChecker::ResultPromiseHandled handled =
        AsyncChecker(this->state).checkDiscardedPromise(expr, callback, at);
    if (handled.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), handled.error().message);
    }
    return;
  }
  if (!type || !type->holdsResults() || isa<AssignExpr>(expr)) {
    return;
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call && (call->builtin() == Builtin::ArrPop || call->builtin() == Builtin::ArrShift ||
               call->builtin() == Builtin::ArrSplice)) {
    return;
  }
  if (!type->nonOptional()->isResult()) {
    if (call && (call->builtin() == Builtin::ArrMap || call->builtin() == Builtin::ArrFlatMap)) {
      this->state.getDiagnostics().error(
          expr->loc(), "the Results are not used, so errors in them would go unchecked; "
                       "handle them, e.g. with 'Result.combine(...)', or keep them in a "
                       "variable");
    }
    return;
  }
  if (callback) {
    this->state.getDiagnostics().error(
        expr->loc(), "the function returns a Result that its caller drops, so an error in it "
                     "would go unchecked; handle it here, e.g. with '.match(...)'");
  } else {
    this->state.getDiagnostics().error(
        expr->loc(), "the Result is not used, so an error in it would go unchecked; handle "
                     "it, e.g. with '.match(...)', or keep it in a variable and check it");
  }
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
