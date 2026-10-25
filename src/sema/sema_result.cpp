// Result of neverthrow, built into the language (docs/errors.md): an Ok with
// a value or an Err with an error, returned as a value instead of thrown.
//
// The type is `Result<T, E>`. `ok(v)` knows only its value: where nothing
// says more, it is a `Result<T, never>`, whose Err cannot happen. Where a
// Result is expected (a declared variable, parameter or return type), `ok(1)`
// takes that type at once; elsewhere the halves meet as the returns of a
// function and the branches of `?:` meet (Sema::unify): `return err("empty")`
// and `return ok(n)` make a `Result<int, string>`. A Result is a value, so it
// converts where each half converts.
//
// `r.isOk()` narrows a variable as `x != undefined` does: where it holds, r
// is a `Result<T, never>`, whose `value` can be read, and where it does not, a
// `Result<never, E>`, whose `error` can.
//
// The methods take functions as the methods of arrays do: an arrow written in
// the call is checked with the types it gets and inlined by CodeGen. safeTry
// takes a generator, which here is an ordinary function called at once:
// `yield* r` takes the value of r, or returns the Err r from the generator.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The names neverthrow exports. Those after safeTry come with exceptions or
// with async.
const llvm::StringLiteral kNeverthrowNames[] = {"ok",
                                                "err",
                                                "Result",
                                                "Ok",
                                                "Err",
                                                "safeTry",
                                                "fromThrowable",
                                                "fromAsyncThrowable",
                                                "ResultAsync",
                                                "okAsync",
                                                "errAsync",
                                                "fromPromise",
                                                "fromSafePromise"};
const llvm::StringLiteral kResultTypes[] = {"Result", "Ok", "Err", "ResultAsync"};

llvm::StringRef findName(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef name) {
  for (llvm::StringLiteral n : names)
    if (n == name)
      return n;
  return {};
}

// What a function given to a method gets for a half: `ok()` holds undefined.
const Type *halfParam(const Type *half) { return half->isVoid() ? Type::undefinedTy() : half; }

// Whether a half can tell a value its type: a hint of never or void cannot.
bool givesType(const Type *half) { return !half->isNever() && !half->isVoid(); }

// The variable a Result comes from, for hints: `r` in `r.map(f)`.
std::string receiverText(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit())
    expr = conv->operand;
  auto *name = dyn_cast<NameExpr>(expr);
  return name && !name->name.starts_with("%") ? name->name.str() : "r";
}

// The value of an expression strips the conversion Sema adds to a narrowed
// variable or to a value given where another type is expected.
const Expr *unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  return expr;
}

} // namespace

//===----------------------------------------------------------------------===//
// Names and types
//===----------------------------------------------------------------------===//

llvm::StringRef Sema::neverthrowName(llvm::StringRef name) const {
  ScopeEntry entry = lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls)
    return {};
  if (module_) {
    const llvm::StringMap<llvm::StringRef> &imported = moduleStates_[module_->order].neverthrow;
    if (auto found = imported.find(name); found != imported.end())
      return found->second;
  }
  return findName(kNeverthrowNames, name);
}

llvm::StringRef Sema::neverthrowTypeName(llvm::StringRef name) const {
  if (lookupType(name))
    return {};
  if (module_) {
    const llvm::StringMap<llvm::StringRef> &imported = moduleStates_[module_->order].neverthrow;
    if (auto found = imported.find(name); found != imported.end())
      return findName(kResultTypes, found->second);
  }
  return findName(kResultTypes, name);
}

void Sema::importNeverthrow(Module *module, const ImportDecl &decl) {
  ModuleState &here = state(module);
  for (const ModuleName &name : decl.names) {
    llvm::StringRef known = findName(kNeverthrowNames, name.name);
    if (known.empty()) {
      diags_.error(name.loc, "'neverthrow' has no export named '" + name.name + "'");
      continue;
    }
    if (auto own = here.declared.find(name.as); own != here.declared.end()) {
      diags_.error(name.asLoc, "import '" + name.as + "' conflicts with the declaration of '" +
                                   name.as +
                                   "' in this file; import it under another name, e.g. "
                                   "'import { " +
                                   name.name + " as " + name.name + "2 } from \"neverthrow\"'");
      diags_.note(own->second, "'" + name.as + "' is declared here");
      continue;
    }
    auto [it, inserted] = here.neverthrow.try_emplace(name.as, known);
    if (!inserted)
      diags_.error(name.asLoc, "duplicate import of '" + name.as + "'");
  }
}

const Type *Sema::resolveResultType(const TypeRef &ref, llvm::StringRef name) {
  if (ref.args.size() != 2) {
    diags_.error(ref.loc, "'" + ref.name +
                              "' takes two type arguments, the value and the error, e.g. '" +
                              ref.name + "<int, string>'");
    return Type::error();
  }
  auto half = [&](const TypeRef &arg) -> const Type * {
    if (arg.kind == TypeRef::Kind::Name && arg.name == "never")
      return Type::never();
    return resolveTypeInner(arg); // void and undefined are halves without a value
  };
  const Type *value = half(ref.args[0]), *error = half(ref.args[1]);
  // An Ok is a Result that cannot be an Err, and an Err one that cannot be an
  // Ok.
  if (name == "Ok")
    error = Type::never();
  else if (name == "Err")
    value = Type::never();
  // A ResultAsync is a Promise of a Result: `await` gives the Result.
  if (name == "ResultAsync")
    return Type::promise(Type::result(value, error));
  return Type::result(value, error);
}

// Why the halves `a` and `b` of Results make no union, for the messages
// that say so: ": <the problem>", or empty.
static std::string noUnion(const Type *a, const Type *b) {
  if (a->isVoid() || b->isVoid())
    return ": one of them is nothing, 'ok()'";
  std::string problem = unionProblem(Type::unionOf({a, b}));
  return problem.empty() ? "" : ": " + problem;
}

const Type *Sema::unifyHalf(const Type *a, const Type *b) {
  if (a == b || b->isNever())
    return a;
  if (a->isNever())
    return b;
  if (a->isVoid() || b->isVoid())
    return nullptr;
  // Errors from different places may be of different types: `E | F`.
  const Type *common = unionUnify(a, b);
  return common && !common->isError() ? common : nullptr;
}

void Sema::neverOperands(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (l->isNever() && r->isNever()) {
    bin->lhs = convert(bin->lhs, Type::doubleTy());
    bin->rhs = convert(bin->rhs, Type::doubleTy());
  } else if (l->isNever() && !r->isError() && !r->isVoid()) {
    bin->lhs = convert(bin->lhs, r);
  } else if (r->isNever() && !l->isError() && !l->isVoid()) {
    bin->rhs = convert(bin->rhs, l);
  }
}

//===----------------------------------------------------------------------===//
// ok, err, safeTry and Result.combine
//===----------------------------------------------------------------------===//

Expr *Sema::checkResultCall(CallExpr *call, NameExpr *name, const Type *expected) {
  llvm::StringRef which = neverthrowName(name->name);
  if (which == "safeTry")
    return checkSafeTry(call, expected);
  if (which == "fromThrowable")
    return checkFromThrowable(call, name->loc());
  if (which == "okAsync" || which == "errAsync" || which == "fromPromise" ||
      which == "fromSafePromise" || which == "fromAsyncThrowable")
    return checkResultAsyncCall(call, name, which, expected); // sema_async.cpp
  if (which != "ok" && which != "err")
    return nullptr;
  return checkOkErr(call, name, which == "ok", expected);
}

Expr *Sema::checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected) {
  if (rejectSpread(call) || !checkArity(call, name->loc(), 0, 1))
    return call;

  // The type expected gives the half this call makes, and the other one.
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && !wanted->isResult())
    wanted = nullptr;
  const Type *wantedHalf = wanted ? (isOk ? wanted->okType() : wanted->errType()) : nullptr;
  const Type *half = Type::voidTy(); // `ok()`
  if (!call->args.empty()) {
    Expr *&arg = call->args[0];
    bool hinted = wantedHalf && givesType(wantedHalf);
    arg = checkExpr(arg, hinted ? wantedHalf : nullptr);
    half = arg->type;
    if (half->isVoid()) {
      diags_.error(arg->loc(), "an expression of type 'void' has no value to keep; write '" +
                                   name->name + "()'");
      half = Type::error();
    } else if (hinted && isImplicitlyConvertible(half, wantedHalf)) {
      arg = convert(arg, wantedHalf);
      half = wantedHalf;
    }
  }
  const Type *other = wanted ? (isOk ? wanted->errType() : wanted->okType()) : Type::never();
  call->builtin = isOk ? Builtin::ResultOk : Builtin::ResultErr;
  call->type = isOk ? Type::result(half, other) : Type::result(other, half);
  return call;
}

Expr *Sema::checkSafeTry(CallExpr *call, const Type *expected) {
  auto *fe = call->args.size() == 1 && !call->hasSpread() ? dyn_cast<FunctionExpr>(call->args[0])
                                                          : nullptr;
  if (!fe || !(fe->fn->isGenerator || fe->fn->safeTry)) {
    diags_.error(call->args.empty() ? call->loc() : call->args[0]->loc(),
                 "safeTry takes a generator function written in the call: 'safeTry(function* () "
                 "{ ... })'");
    for (Expr *&arg : call->args)
      if (!isa<FunctionExpr>(arg))
        arg = checkExpr(arg);
    return call;
  }
  FunctionDecl *fn = fe->fn;
  if (!fn->params.empty())
    diags_.error(fn->params[0].loc, "the generator of safeTry takes no parameters");
  // What it returns is the Result of safeTry.
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  Signature sig;
  sig.pad = false;
  sig.ret = wanted && wanted->isResult() ? wanted : nullptr;
  fn->nonEscaping = true; // called during the call, once
  // Not a generator: it runs to its end at once (see FunctionDecl::safeTry).
  fn->safeTry = true;
  fn->isGenerator = false;
  call->args[0] = checkFunctionExpr(fe, sig.ret ? &sig : nullptr);
  call->builtin = Builtin::SafeTry;
  calledFunction();

  const Type *ret = fn->sym->ret;
  if (!ret || ret->isError())
    return call;
  if (!ret->isResult()) {
    diags_.error(fn->loc(), ret->isOptional() && ret->inner()->isResult()
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

const Type *Sema::generatorResult(const FunctionDecl *fn, const Type *returned) {
  if (!returned->isResult())
    return returned; // reported by checkSafeTry
  const Type *result = returned;
  visitExprs(
      fn->body,
      [&](const Expr *e) {
        auto *yield = dyn_cast<YieldExpr>(e);
        if (!result || !yield || !yield->delegate || !yield->value->type->isResult())
          return;
        const Type *error = yield->value->type->errType();
        const Type *common = unifyHalf(result->errType(), error);
        if (!common) {
          diags_.error(yield->loc(), "'yield*' returns the error '" + error->name() +
                                         "' here, and the generator returns errors of '" +
                                         result->errType()->name() +
                                         "' elsewhere, which no union holds" +
                                         noUnion(result->errType(), error) +
                                         "; convert one with 'mapErr'");
          result = nullptr;
          return;
        }
        result = Type::result(result->okType(), common);
      },
      /*intoFunctions=*/false);
  return result ? result : Type::error();
}

Expr *Sema::checkSafeTryYield(YieldExpr *yield) {
  yield->type = Type::error();
  const FuncSymbol *fn = currentFunction_;
  if (!yield->delegate) {
    if (yield->value)
      yield->value = checkExpr(yield->value);
    diags_.error(yield->loc(), "the generator of safeTry gives no values: 'yield x' has no one to "
                               "take x; 'yield* r' takes the value of the Result r");
    return yield;
  }
  // `yield* r.safeUnwrap()`, as neverthrow wrote it before, is `yield* r`.
  if (auto *call = dyn_cast<CallExpr>(yield->value);
      call && call->args.empty() && !call->optional)
    if (auto *member = dyn_cast<MemberExpr>(call->callee); member && member->name == "safeUnwrap")
      yield->value = member->object;
  yield->value = checkExpr(yield->value);
  const Type *type = yield->value->type;
  if (type->isError())
    return yield;
  if (!type->isResult()) {
    diags_.error(yield->value->loc(),
                 "'yield*' in the generator of safeTry takes a Result, found '" + type->name() +
                     "'; a generator that gives values is called directly, not by safeTry");
    return yield;
  }
  // An Err returns from the generator: its error must fit what it returns.
  if (const Type *ret = fn->ret;
      ret && ret->isResult() &&
      !isImplicitlyConvertible(Type::result(Type::never(), type->errType()), ret))
    diags_.error(yield->loc(), "'yield*' returns the error '" + type->errType()->name() +
                                   "' when the Result is an Err, but the generator returns '" +
                                   ret->name() + "'; convert the error with 'mapErr'");
  // `yield* err(e)` always returns, as `return err(e)` does.
  if (type->okType()->isNever())
    flow_.unreachable = true;
  yield->type = halfParam(type->okType());
  return yield;
}

// A `yield*` that returns from the generator releases the variables, as a
// `return` does, but not what the statement around it has computed before
// it: in `f(s.trim(), yield* r)` the trimmed string is held for the call. So
// `yield*` may come only where nothing computed before it in its statement
// holds a reference: at the start of a statement or an initializer, in
// arithmetic on numbers, in the arguments of a call after numbers and the
// like, in the value of ok(), in a branch of `?:`.
void Sema::checkYieldPlacement(const Stmt *stmt) {
  if (!stmt)
    return;
  auto expr = [&](const Expr *e) {
    if (e)
      checkYieldPlacement(e, /*safe=*/true);
  };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body)
      checkYieldPlacement(s);
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
      expr(d.init);
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
    // The discriminant waits in a variable of the switch, which a return
    // releases.
    expr(cast<SwitchStmt>(stmt)->discriminant);
    for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
      expr(c.test);
      for (const Stmt *s : c.body)
        checkYieldPlacement(s);
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
    if (s->handler)
      checkYieldPlacement(s->handler);
    if (s->finalizer)
      checkYieldPlacement(s->finalizer);
    break;
  }
  default:
    break; // a function declared inside is a function of its own
  }
}

void Sema::checkYieldPlacement(const Expr *expr, bool safe) {
  auto sub = [&](const Expr *e, bool s) {
    if (e)
      checkYieldPlacement(e, s);
  };
  // Whether the value of `e` holds no reference while what follows it runs:
  // a string literal is a constant, which is not counted.
  auto plain = [](const Expr *e) {
    const Expr *value = unconverted(e);
    auto *tmpl = dyn_cast<TemplateExpr>(value);
    return !e->type || !e->type->isManaged() || isa<StringLitExpr>(value) ||
           (tmpl && tmpl->exprs.empty());
  };
  switch (expr->kind()) {
  case Expr::Kind::Yield: {
    auto *yield = cast<YieldExpr>(expr);
    if (yield->delegate && !safe)
      diags_.error(yield->loc(),
                   "'yield*' cannot be used here: when the Result is an Err it returns "
                   "from the generator, and a value computed before it in this "
                   "statement would be lost; take the value first, as in 'const x = "
                   "yield* r'");
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
    // `&&`, `||` and `??` have released their left side, or it holds
    // nothing, when the right one runs.
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
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs)
      sub(e, safe);
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
    // A number goes into the text as it is; a string is held.
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
    if (!toVariable)
      sub(assign->target, false);
    sub(assign->value, safe && toVariable && (!assign->op || !assign->type->isString()));
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    // The receiver of a method and a function value are held while the
    // arguments are computed, but not a namespace such as Math or console.
    auto *member = dyn_cast<MemberExpr>(call->callee);
    auto *space = member ? dyn_cast<NameExpr>(member->object) : nullptr;
    bool namespaced = space && !space->var;
    bool held =
        (member && !namespaced) || (!member && !call->func && call->builtin == Builtin::None);
    if (member && !namespaced)
      sub(member->object, safe);
    else if (!member && !call->func && call->builtin == Builtin::None)
      sub(call->callee, safe);
    for (const Expr *arg : call->args) {
      sub(arg, safe && !held);
      held |= !plain(arg);
    }
    break;
  }
  case Expr::Kind::ArrayLit:
    // The new array exists before its elements are computed.
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
      sub(e.expr, false);
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      sub(p.key, false);
      sub(p.value, false);
    }
    break;
  case Expr::Kind::New:
    for (const Expr *arg : cast<NewExpr>(expr)->args)
      sub(arg, false);
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain, false);
    break;
  case Expr::Kind::Update:
    sub(cast<UpdateExpr>(expr)->target, false);
    break;
  default:
    break; // literals, names, and functions, which are functions of their own
  }
}

Expr *Sema::checkResultStatic(CallExpr *call, MemberExpr *member) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (!name || call->optional || member->optional || neverthrowName(name->name) != "Result")
    return nullptr;
  llvm::StringRef method = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (method == "fromThrowable")
    return checkFromThrowable(call, member->loc());
  if (method != "combine" && method != "combineWithAllErrors") {
    diags_.error(member->loc(), "'" + name->name + "." + method +
                                    "' does not exist; Result has 'combine' and "
                                    "'combineWithAllErrors'");
    checkArgsLoosely();
    return call;
  }
  if (rejectSpread(call) || !checkArity(call, member->loc(), 1, 1))
    return call;
  bool all = method == "combineWithAllErrors";
  call->builtin = all ? Builtin::ResultCombineWithAllErrors : Builtin::ResultCombine;
  std::string what = name->name.str() + "." + method.str();
  // The values (or errors) would make an array of undefined.
  auto holds = [&](const Type *result, const Expr *at) {
    if (givesType(result->okType()) && !(all && result->errType()->isVoid()))
      return true;
    diags_.error(at->loc(), "'" + what + "' needs Results that hold " +
                                (givesType(result->okType()) ? "errors" : "values") + ", found '" +
                                result->name() +
                                "'; check them one by one, e.g. with 'every(r => r.isOk())'");
    return false;
  };
  auto errorsOf = [&](const Type *error) {
    return !all || error->isNever() ? error : Type::array(error);
  };

  // A list of Results with values of different types is a tuple of them, as
  // neverthrow types it: `Result.combine([parseName(s), parseAge(s)])` is a
  // `Result<[string, int], E>`.
  Expr *&arg = call->args[0];
  if (auto *lit = dyn_cast<ArrayLitExpr>(arg);
      lit && !lit->elements.empty() &&
      llvm::none_of(lit->elements,
                    [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; })) {
    std::vector<const Type *> results;
    for (ArrayLitExpr::Element &e : lit->elements) {
      e.expr = checkExpr(e.expr);
      results.push_back(e.expr->type);
    }
    if (llvm::any_of(results, [](const Type *t) { return t->isError(); }))
      return call;
    auto other = llvm::find_if(results, [](const Type *t) { return !t->isResult(); });
    if (other != results.end()) {
      diags_.error(lit->elements[other - results.begin()].expr->loc(),
                   "'" + what + "' takes Results, found '" + (*other)->name() + "'");
      return call;
    }
    // Values of one type make an array; their errors may differ, `E | F`.
    auto sameValues = [](const Type *a, const Type *b) -> const Type * {
      const Type *x = a->okType(), *y = b->okType();
      const Type *value = x == y || y->isNever() ? x
                          : x->isNever()          ? y
                          : x->isVoid() || y->isVoid() ? nullptr
                                                        : unify(x, y);
      const Type *error = unifyHalf(a->errType(), b->errType());
      return value && !value->isError() && error ? Type::result(value, error) : nullptr;
    };
    const Type *common = results[0];
    for (const Type *t : results)
      common = common ? sameValues(common, t) : nullptr;
    if (common && !common->isError()) {
      // Of one type: an array.
      if (!holds(common, arg))
        return call;
      for (ArrayLitExpr::Element &e : lit->elements)
        e.expr = convert(e.expr, common);
      lit->type = Type::array(common);
      call->type = Type::result(Type::array(common->okType()), errorsOf(common->errType()));
      return call;
    }
    std::vector<const Type *> values;
    const Type *error = Type::never();
    for (size_t i = 0; i < results.size(); ++i) {
      if (!holds(results[i], lit->elements[i].expr))
        return call;
      values.push_back(results[i]->okType());
      const Type *before = error;
      if (!(error = unifyHalf(error, results[i]->errType()))) {
        diags_.error(lit->elements[i].expr->loc(),
                     "the Results given to '" + what + "' have errors that no union holds" +
                         noUnion(before, results[i]->errType()) +
                         "; convert them with 'mapErr'");
        return call;
      }
    }
    lit->type = Type::tuple(results);
    call->type = Type::result(Type::tuple(values), errorsOf(error));
    return call;
  }

  arg = checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError())
    return call;
  if (!type->isArray() || !type->inner()->isResult()) {
    diags_.error(arg->loc(),
                 "'" + what + "' takes an array of Results, found '" + type->name() + "'");
    return call;
  }
  const Type *element = type->inner();
  if (!holds(element, arg))
    return call;
  call->type = Type::result(Type::array(element->okType()), errorsOf(element->errType()));
  return call;
}

//===----------------------------------------------------------------------===//
// Methods and members
//===----------------------------------------------------------------------===//

Expr *Sema::checkResultMember(MemberExpr *member) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  std::string r = receiverText(member->object);
  if (name == "value") {
    if (type->errType()->isNever()) {
      member->type = halfParam(type->okType());
      return member;
    }
    diags_.error(member->loc(),
                 "'value' is there only when the Result is an Ok: check it first, 'if (" + r +
                     ".isOk())', or use 'match' or 'unwrapOr'");
  } else if (name == "error") {
    if (type->okType()->isNever()) {
      member->type = halfParam(type->errType());
      return member;
    }
    diags_.error(member->loc(),
                 "'error' is there only when the Result is an Err: check it first, 'if (" + r +
                     ".isErr())', or use 'match'");
  } else {
    static const char *const methods[] = {"isOk",    "isErr",  "map",       "mapErr",
                                          "andThen", "orElse", "match",     "unwrapOr",
                                          "andTee",  "orTee",  "andThrough"};
    if (llvm::is_contained(methods, name))
      diags_.error(member->loc(),
                   "'" + name + "' is a method of Result; call it: '" + r + "." + name + "(...)'");
    else
      diags_.error(member->loc(),
                   "property '" + name + "' does not exist on type '" + type->name() + "'");
  }
  return member;
}

Expr *Sema::checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *type = member->object->type;
  const Type *value = type->okType(), *error = type->errType();
  // A variable narrowed by isOk() keeps the type of its error for the
  // methods, as in TypeScript: a function given for it gets that type,
  // though it is never called.
  if (auto *conv = dyn_cast<CastExpr>(member->object);
      conv && conv->isImplicit() && conv->operand->type->isResult()) {
    if (value->isNever())
      value = conv->operand->type->okType();
    if (error->isNever())
      error = conv->operand->type->errType();
  }
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (rejectSpread(call))
    return call;
  if (name == "_unsafeUnwrap" || name == "_unsafeUnwrapErr") {
    // The half asked for, or an Error thrown (neverthrow's ErrorConfig only
    // chooses whether its own error object has a stack; an Error always has).
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    bool unwrapErr = name == "_unsafeUnwrapErr";
    call->builtin = unwrapErr ? Builtin::ResultUnsafeUnwrapErr : Builtin::ResultUnsafeUnwrap;
    call->type = halfParam(unwrapErr ? error : value);
    return call;
  }
  if (name == "asyncMap" || name == "asyncAndThen")
    return checkResultAsyncMethod(call, member, name); // sema_async.cpp
  if (name == "asyncAndThrough") {
    diags_.error(member->loc(), "'asyncAndThrough' is not supported yet; use 'asyncAndThen', "
                                "which gives the Result its function gives");
    checkArgsLoosely();
    return call;
  }
  if (name == "safeUnwrap") {
    diags_.error(member->loc(), "'safeUnwrap()' is needed only after 'yield*' in safeTry, where "
                                "'yield* r' does the same");
    checkArgsLoosely();
    return call;
  }
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args;
  };
  static const Method methods[] = {
      {"isOk", Builtin::ResultIsOk, 0},
      {"isErr", Builtin::ResultIsErr, 0},
      {"map", Builtin::ResultMap, 1},
      {"mapErr", Builtin::ResultMapErr, 1},
      {"andThen", Builtin::ResultAndThen, 1},
      {"orElse", Builtin::ResultOrElse, 1},
      {"match", Builtin::ResultMatch, 2},
      {"unwrapOr", Builtin::ResultUnwrapOr, 1},
      {"andTee", Builtin::ResultAndTee, 1},
      {"orTee", Builtin::ResultOrTee, 1},
      {"andThrough", Builtin::ResultAndThrough, 1},
  };
  const Method *method = nullptr;
  for (const Method &m : methods)
    if (m.name == name)
      method = &m;
  if (!method) {
    diags_.error(member->loc(),
                 "property '" + name + "' does not exist on type '" + type->name() + "'");
    checkArgsLoosely();
    return call;
  }
  if (!checkArity(call, member->loc(), method->args, method->args))
    return call;
  Builtin builtin = method->builtin;
  call->builtin = builtin;

  if (builtin == Builtin::ResultIsOk || builtin == Builtin::ResultIsErr) {
    call->type = Type::boolTy();
    return call;
  }

  if (builtin == Builtin::ResultUnwrapOr) {
    // The value of an Ok, or the argument.
    Expr *&arg = call->args[0];
    arg = checkExpr(arg, givesType(value) ? value : nullptr);
    const Type *other = arg->type;
    if (other->isError())
      return call;
    const Type *result = other->isVoid() ? nullptr : unionUnify(value, other);
    if (!result || result->isError()) {
      diags_.error(arg->loc(),
                   "'unwrapOr' gives '" + value->name() + "' for an Ok and '" + other->name() +
                       "' otherwise, and no type holds both; say what each gives with 'match'");
      return call;
    }
    arg = convert(arg, result);
    call->type = result;
    return call;
  }

  // The methods that take a function: what it gets, and for an arrow written
  // here, what it should return where that tells its literals their types.
  bool onError = builtin == Builtin::ResultMapErr || builtin == Builtin::ResultOrElse ||
                 builtin == Builtin::ResultOrTee;
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && wanted->isError())
    wanted = nullptr;
  auto check = [&](Expr *&arg, const Type *half, const Type *ret) -> const Type * {
    if (half->isNever() && !isa<FunctionExpr>(arg)) {
      // Never called, as the Result cannot have that half: any function
      // will do (and CodeGen leaves the call out).
      arg = checkExpr(arg);
      if (arg->type->isFunction())
        return arg->type->ret();
      if (!arg->type->isError())
        diags_.error(arg->loc(),
                     "'" + name + "' needs a function, found '" + arg->type->name() + "'");
      return nullptr;
    }
    Signature sig;
    sig.pad = false;
    sig.params = {halfParam(half)};
    sig.voidResult = builtin == Builtin::ResultAndTee || builtin == Builtin::ResultOrTee;
    if (isa<FunctionExpr>(arg) && !sig.voidResult)
      sig.ret = ret;
    arg = checkFunctionArg(arg, sig, 1, name);
    return arg->type->isFunction() ? arg->type->ret() : nullptr;
  };
  const Type *wantedResult = wanted && wanted->isResult() ? wanted : nullptr;
  const Type *ret = nullptr;
  switch (builtin) {
  case Builtin::ResultMap:
  case Builtin::ResultMapErr: {
    // Objects, and Results, take their type from the place, as with map of
    // arrays.
    const Type *half =
        wantedResult ? (onError ? wantedResult->errType() : wantedResult->okType()) : nullptr;
    ret = check(call->args[0], onError ? error : value,
                half && (holdsObjects(half) || holdsResults(half)) ? half : nullptr);
    break;
  }
  case Builtin::ResultAndThen:
  case Builtin::ResultOrElse:
    ret = check(call->args[0], onError ? error : value, wantedResult);
    break;
  case Builtin::ResultMatch: {
    const Type *both = wanted && !wanted->isVoid() ? wanted : nullptr;
    const Type *a = check(call->args[0], value, both);
    const Type *b = check(call->args[1], error, both);
    calledFunction();
    if (!a || !b)
      return call;
    if (a->isVoid() && b->isVoid()) {
      call->type = a;
      return call;
    }
    const Type *result = a->isVoid() || b->isVoid() ? nullptr : unionUnify(a, b);
    if (!result || result->isError()) {
      diags_.error(member->loc(), "the functions given to 'match' return '" + a->name() +
                                      "' and '" + b->name() + "', and no type holds both");
      return call;
    }
    call->type = result;
    return call;
  }
  default: // andTee, orTee, andThrough
    ret = check(call->args[0], onError ? error : value, nullptr);
    break;
  }
  calledFunction(); // the function may change anything
  if (!ret)
    return call;

  auto needsResult = [&]() {
    if (ret->isResult())
      return true;
    diags_.error(call->args[0]->loc(),
                 "the function given to '" + name +
                     "' must return a Result, 'ok(...)' or 'err(...)', found '" + ret->name() +
                     "'" +
                     (builtin == Builtin::ResultAndThen ? "; to change the value, use 'map'" : ""));
    return false;
  };
  auto oneError = [&](const Type *a, const Type *b) {
    const Type *common = unifyHalf(a, b);
    if (!common)
      diags_.error(call->args[0]->loc(),
                   "the Result has errors of '" + a->name() + "' and the function given to '" +
                       name + "' returns errors of '" + b->name() + "', which no union holds" +
                       noUnion(a, b) + "; convert one with 'mapErr'");
    return common;
  };
  switch (builtin) {
  case Builtin::ResultMap:
    call->type = Type::result(ret, error);
    break;
  case Builtin::ResultMapErr:
    call->type = Type::result(value, ret);
    break;
  case Builtin::ResultAndThen:
    if (!needsResult())
      return call;
    if (const Type *common = oneError(error, ret->errType()))
      call->type = Type::result(ret->okType(), common);
    break;
  case Builtin::ResultAndThrough:
    if (!needsResult())
      return call;
    if (const Type *common = oneError(error, ret->errType()))
      call->type = Type::result(value, common);
    break;
  case Builtin::ResultOrElse: {
    if (!needsResult())
      return call;
    const Type *common = unifyHalf(value, ret->okType());
    if (!common) {
      diags_.error(call->args[0]->loc(),
                   "the Result has values of '" + value->name() +
                       "' and the function given to 'orElse' returns values of '" +
                       ret->okType()->name() + "', which no union holds" +
                       noUnion(value, ret->okType()));
      return call;
    }
    call->type = Type::result(common, ret->errType());
    break;
  }
  default: // andTee and orTee give the Result as it is
    call->type = type;
    break;
  }
  return call;
}

//===----------------------------------------------------------------------===//
// Using Results
//===----------------------------------------------------------------------===//

void Sema::checkDiscarded(const Expr *expr, bool callback, llvm::SMLoc at) {
  const Type *type = expr->type;
  // A Promise nothing waits for (sema_async.cpp).
  if (type && holdsPromises(type)) {
    checkDiscardedPromise(expr, callback, at);
    return;
  }
  // `r = ok(1)` keeps it.
  if (!type || !holdsResults(type) || isa<AssignExpr>(expr))
    return;
  // What pop(), shift() and splice() take out of an array was kept there,
  // and is removed on purpose; reverse() and the like give the array itself.
  auto *call = dyn_cast<CallExpr>(expr);
  if (call && (call->builtin == Builtin::ArrPop || call->builtin == Builtin::ArrShift ||
               call->builtin == Builtin::ArrSplice))
    return;
  if (!type->nonOptional()->isResult()) {
    // New Results nobody takes: `files.map(f => save(f))`.
    if (call && (call->builtin == Builtin::ArrMap || call->builtin == Builtin::ArrFlatMap))
      diags_.error(expr->loc(), "the Results are not used, so errors in them would go unchecked; "
                                "handle them, e.g. with 'Result.combine(...)', or keep them in a "
                                "variable");
    return;
  }
  if (callback)
    diags_.error(expr->loc(),
                 "the function returns a Result that its caller drops, so an error in it "
                 "would go unchecked; handle it here, e.g. with '.match(...)'");
  else
    diags_.error(expr->loc(),
                 "the Result is not used, so an error in it would go unchecked; handle "
                 "it, e.g. with '.match(...)', or keep it in a variable");
}

void Sema::addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (!call || (call->builtin != Builtin::ResultIsOk && call->builtin != Builtin::ResultIsErr))
    return;
  auto *name = dyn_cast<NameExpr>(unconverted(cast<MemberExpr>(call->callee)->object));
  if (!name || !name->var || !name->var->type || !name->var->type->isResult())
    return;
  bool isOk = call->builtin == Builtin::ResultIsOk;
  Flow &ok = isOk ? whenTrue : whenFalse, &err = isOk ? whenFalse : whenTrue;
  ok.okResults.insert(name->var);
  ok.errResults.erase(name->var);
  err.errResults.insert(name->var);
  err.okResults.erase(name->var);
}

// `Result.fromThrowable(f, toError?)`: a function with the parameters of f
// that calls it and gives what it returns as an Ok, and what it throws as an
// Err, an Error or what toError makes of it. f and toError are evaluated once,
// here (see CodeGen::emitFromThrowable).
Expr *Sema::checkFromThrowable(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, loc, 1, 2))
    return call;
  Expr *&fn = call->args[0];
  fn = checkExpr(fn);
  if (call->args.size() > 1 && !isa<FunctionExpr>(call->args[1]))
    call->args[1] = checkExpr(call->args[1]);
  const Type *fnType = fn->type;
  if (fnType->isError())
    return call;
  if (!fnType->isFunction()) {
    diags_.error(fn->loc(), "'fromThrowable' takes a function, found '" + fnType->name() + "'");
    return call;
  }
  // An async function throws nothing: its errors reject its Promise.
  if (fnType->ret()->isPromise()) {
    diags_.error(fn->loc(), "'fromThrowable' of a function that gives a Promise would give an Ok "
                            "with the Promise, as the function's errors reject the Promise; use "
                            "'ResultAsync.fromThrowable(f)', which gives a ResultAsync");
    return call;
  }
  const Type *errType = errorClass_->type;
  if (call->args.size() > 1) {
    Signature sig;
    sig.params = {errorClass_->type};
    sig.minArgs = 1;
    // It is kept in the function made here, so an arrow's environment
    // cannot live on the stack (as checkFunctionArg would have it).
    Expr *&toError = call->args[1];
    if (auto *fe = dyn_cast<FunctionExpr>(toError))
      toError = checkFunctionExpr(fe, &sig);
    const Type *toType = toError->type;
    if (toType->isError())
      return call;
    bool takesError = toType->isFunction() && toType->minArgs() <= 1 &&
                      (toType->params().empty() || toType->params()[0] == errorClass_->type ||
                       toType->params()[0] == Type::optional(errorClass_->type));
    if (!takesError) {
      diags_.error(toError->loc(), "the second argument of 'fromThrowable' turns the Error into "
                                   "the error of the Result: a function that takes an Error, e.g. "
                                   "'(e) => e.message'");
      return call;
    }
    errType = toError->type->ret();
    if (errType->isVoid()) {
      diags_.error(toError->loc(), "the function that turns the Error into the error of the "
                                   "Result must return it");
      return call;
    }
  }
  call->builtin = Builtin::ResultFromThrowable;
  call->type = Type::function(fnType->params(), fnType->minArgs(), fnType->hasRest(),
                              Type::result(fnType->ret(), errType));
  return call;
}

} // namespace tz
