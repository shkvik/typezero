#include "compiler/library/result/safe_try_checker/safe_try_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const Type *halfParam(const Type *half) { return half->isVoid() ? Type::undefinedTy() : half; }

const Expr *unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

}

static std::string noUnion(const Type *a, const Type *b) {
  if (a->isVoid() || b->isVoid()) {
    return ": one of them is nothing, 'ok()'";
  }
  std::string problem = unionProblem(Type::unionOf({a, b}));

  return problem.empty() ? "" : ": " + problem;
}

SafeTryChecker::SafeTryChecker(Checker &checker) : checker_(checker) {}

Expr *SafeTryChecker::checkSafeTry(CallExpr *call, const Type *expected) {
  auto *fe = call->args.size() == 1 && !call->hasSpread() ? dyn_cast<FunctionExpr>(call->args[0])
                                                          : nullptr;
  if (!fe || !(fe->fn->isGenerator || fe->fn->safeTry)) {
    checker_.diags_.error(
        call->args.empty() ? call->loc() : call->args[0]->loc(),
        "safeTry takes a generator function written in the call: 'safeTry(function* () "
        "{ ... })'");
    for (Expr *&arg : call->args) {
      if (!isa<FunctionExpr>(arg)) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
    }
    return call;
  }
  FunctionDecl *fn = fe->fn;
  if (!fn->params.empty()) {
    checker_.diags_.error(fn->params[0].loc, "the generator of safeTry takes no parameters");
  }
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  Checker::Signature sig;
  sig.pad = false;
  sig.ret = wanted && wanted->isResult() ? wanted : nullptr;
  fn->nonEscaping = true;
  fn->safeTry = true;
  fn->isGenerator = false;
  call->args[0] = checker_.closureChecker().checkFunctionExpr(fe, sig.ret ? &sig : nullptr);
  call->builtin = Builtin::SafeTry;
  checker_.flowChecker().calledFunction();

  const Type *ret = fn->sym->ret;
  if (!ret || ret->isError()) {
    return call;
  }
  if (!ret->isResult()) {
    checker_.diags_.error(fn->loc(),
                          ret->isOptional() && ret->inner()->isResult()
                              ? "the generator of safeTry must return a Result on every path, "
                                "e.g. 'return ok(value)' at its end"
                              : "the generator of safeTry must return a Result, e.g. 'return "
                                "ok(value)'");
    return call;
  }
  call->type = ret;
  checkYieldPlacement(fn->body);

  return call;
}

const Type *SafeTryChecker::generatorResult(const FunctionDecl *fn, const Type *returned) {
  if (!returned->isResult()) {
    return returned;
  }
  const Type *result = returned;
  visitExprs(
      fn->body,
      [&](const Expr *e) {
        auto *yield = dyn_cast<YieldExpr>(e);
        if (!result || !yield || !yield->delegate || !yield->value->type->isResult()) {
          return;
        }
        const Type *error = yield->value->type->errType();
        const Type *common = ResultTypes::unifyHalf(result->errType(), error);
        if (!common) {
          checker_.diags_.error(
              yield->loc(), "'yield*' returns the error '" + error->name() +
                                "' here, and the generator returns errors of '" +
                                result->errType()->name() + "' elsewhere, which no union holds" +
                                noUnion(result->errType(), error) + "; convert one with 'mapErr'");
          result = nullptr;
          return;
        }
        result = Type::result(result->okType(), common);
      },
      false);

  return result ? result : Type::error();
}

Expr *SafeTryChecker::checkSafeTryYield(YieldExpr *yield) {
  yield->type = Type::error();
  const FuncSymbol *fn = checker_.currentFunction_;
  if (!yield->delegate) {
    if (yield->value) {
      yield->value = checker_.exprChecker().checkExpr(yield->value);
    }
    checker_.diags_.error(yield->loc(),
                          "the generator of safeTry gives no values: 'yield x' has no one to "
                          "take x; 'yield* r' takes the value of the Result r");
    return yield;
  }
  if (auto *call = dyn_cast<CallExpr>(yield->value);
      call && call->args.empty() && !call->optional) {
    if (auto *member = dyn_cast<MemberExpr>(call->callee); member && member->name == "safeUnwrap") {
      yield->value = member->object;
    }
  }
  yield->value = checker_.exprChecker().checkExpr(yield->value);
  const Type *type = yield->value->type;
  if (type->isError()) {
    return yield;
  }
  if (!type->isResult()) {
    checker_.diags_.error(
        yield->value->loc(),
        "'yield*' in the generator of safeTry takes a Result, found '" + type->name() +
            "'; a generator that gives values is called directly, not by safeTry");
    return yield;
  }
  if (const Type *ret = fn->ret;
      ret && ret->isResult() &&
      !TypeRelations::isImplicitlyConvertible(Type::result(Type::never(), type->errType()), ret)) {
    checker_.diags_.error(yield->loc(),
                          "'yield*' returns the error '" + type->errType()->name() +
                              "' when the Result is an Err, but the generator returns '" +
                              ret->name() + "'; convert the error with 'mapErr'");
  }
  if (type->okType()->isNever()) {
    checker_.flow_.unreachable = true;
  }
  yield->type = halfParam(type->okType());

  return yield;
}

void SafeTryChecker::checkYieldPlacement(const Stmt *stmt) {
  if (!stmt) {
    return;
  }
  auto expr = [&](const Expr *e) {
    if (e) {
      checkYieldPlacement(e, true);
    }
  };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      checkYieldPlacement(s);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      expr(d.init);
    }
    break;
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    expr(s->cond);
    checkYieldPlacement(s->thenStmt);
    checkYieldPlacement(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(stmt)->cond);
    checkYieldPlacement(cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    checkYieldPlacement(cast<DoWhileStmt>(stmt)->body);
    expr(cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    checkYieldPlacement(s->init);
    expr(s->cond);
    expr(s->step);
    checkYieldPlacement(s->body);
    break;
  }
  case Stmt::Kind::ForOf:
    expr(cast<ForOfStmt>(stmt)->iterable);
    checkYieldPlacement(cast<ForOfStmt>(stmt)->body);
    break;
  case Stmt::Kind::Switch:
    expr(cast<SwitchStmt>(stmt)->discriminant);
    for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
      expr(c.test);
      for (const Stmt *s : c.body) {
        checkYieldPlacement(s);
      }
    }
    break;
  case Stmt::Kind::Labeled:
    checkYieldPlacement(cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    expr(cast<ReturnStmt>(stmt)->value);
    break;
  case Stmt::Kind::Throw:
    expr(cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    checkYieldPlacement(s->block);
    if (s->handler) {
      checkYieldPlacement(s->handler);
    }
    if (s->finalizer) {
      checkYieldPlacement(s->finalizer);
    }
    break;
  }
  default:
    break;
  }
}

void SafeTryChecker::checkYieldPlacement(const Expr *expr, bool safe) {
  auto sub = [&](const Expr *e, bool s) {
    if (e) {
      checkYieldPlacement(e, s);
    }
  };
  auto plain = [](const Expr *e) {
    const Expr *value = unconverted(e);
    auto *tmpl = dyn_cast<TemplateExpr>(value);

    return !e->type || !e->type->isManaged() || isa<StringLitExpr>(value) ||
           (tmpl && tmpl->exprs.empty());
  };
  switch (expr->kind()) {
  case Expr::Kind::Yield: {
    auto *yield = cast<YieldExpr>(expr);
    if (yield->delegate && !safe) {
      checker_.diags_.error(yield->loc(),
                            "'yield*' cannot be used here: when the Result is an Err it returns "
                            "from the generator, and a value computed before it in this "
                            "statement would be lost; take the value first, as in 'const x = "
                            "yield* r'");
    }
    sub(yield->value, safe);
    break;
  }
  case Expr::Kind::Unary:
    sub(cast<UnaryExpr>(expr)->operand, safe);
    break;
  case Expr::Kind::Await:
    sub(cast<AwaitExpr>(expr)->value, safe);
    break;
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    bool passes =
        bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr || bin->op == BinaryOp::Nullish;
    sub(bin->lhs, safe);
    sub(bin->rhs, safe && (passes || plain(bin->lhs)));
    break;
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    sub(cond->cond, safe);
    sub(cond->thenExpr, safe);
    sub(cond->elseExpr, safe);
    break;
  }
  case Expr::Kind::Cast:
    sub(cast<CastExpr>(expr)->operand, safe);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
      sub(e, safe);
    }
    break;
  case Expr::Kind::Member:
    sub(cast<MemberExpr>(expr)->object, safe);
    break;
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    sub(index->object, safe);
    sub(index->index, safe && plain(index->object));
    break;
  }
  case Expr::Kind::Template: {
    bool held = false;
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      sub(e, safe && !held);
      held |= !plain(unconverted(e));
    }
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    bool toVariable = isa<NameExpr>(assign->target);
    if (!toVariable) {
      sub(assign->target, false);
    }
    sub(assign->value, safe && toVariable && (!assign->op || !assign->type->isString()));
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    auto *member = dyn_cast<MemberExpr>(call->callee);
    auto *space = member ? dyn_cast<NameExpr>(member->object) : nullptr;
    bool namespaced = space && !space->var;
    bool held =
        (member && !namespaced) || (!member && !call->func && call->builtin == Builtin::None);
    if (member && !namespaced) {
      sub(member->object, safe);
    } else if (!member && !call->func && call->builtin == Builtin::None) {
      sub(call->callee, safe);
    }
    for (const Expr *arg : call->args) {
      sub(arg, safe && !held);
      held |= !plain(arg);
    }
    break;
  }
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
      sub(e.expr, false);
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      sub(p.key, false);
      sub(p.value, false);
    }
    break;
  case Expr::Kind::New:
    for (const Expr *arg : cast<NewExpr>(expr)->args) {
      sub(arg, false);
    }
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain, false);
    break;
  case Expr::Kind::Update:
    sub(cast<UpdateExpr>(expr)->target, false);
    break;
  default:
    break;
  }
}

}
