// Functions that test (docs/narrowing.md, sections 1 and 2).
//
// A function that returns a boolean narrows where it is called in a
// condition, as the condition in its body would: `if (isCircle(s)) s.r`.
// While its body is checked, each `return` records what holds about the
// parameters when it gives true and when it gives false (the flow its value
// leaves for each, see checkBranching); the summary of a function joins
// them, as control merges. A call then brings the summary's facts to its
// arguments: the types of variables, the fields that hold a value, the
// facts about ints and lengths that prove indices. A function whose body
// proves little narrows little; nothing is narrowed that the body does not
// show.
//
// A predicate written in the return type, `x is T`, `asserts x is T` and
// `this is T`, is checked against the body: each return that may give true
// (or each way out, for asserts) must have shown x to be T.

#include "compiler/check/sema/sema.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The variable an argument reads, through the conversions of narrowing and
// of passing it.
const VarSymbol *argumentVariable(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit)
      return nullptr;
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  return name ? name->var : nullptr;
}

// An argument as a term of the facts: a variable, or a constant (an integer
// literal); none for anything else.
std::optional<Linear> argumentTerm(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      return std::nullopt;
    expr = conv->operand;
  }
  bool negated = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    negated = true;
    expr = unary->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr); lit && lit->value < (uint64_t(1) << 31))
    return Linear{Term::zero(), negated ? -int64_t(lit->value) : int64_t(lit->value)};
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var && !negated)
    return Linear{Term::of(name->var), 0};
  return std::nullopt;
}

} // namespace

const Type *Sema::resolveReturnType(FunctionDecl *fn, FuncSymbol *sym) {
  const TypeRef &ref = *fn->returnType;
  if (ref.kind != TypeRef::Kind::Predicate)
    return resolveType(ref, /*allowVoid=*/true);
  int param = -2;
  if (ref.name == "this") {
    if (fn->cls && !fn->isStatic && !fn->isConstructor)
      param = -1;
    else if (sym)
      diags_.error(ref.loc, "'this is T' can only be the return type of a method");
  } else {
    for (size_t i = 0; i < fn->params.size(); ++i)
      if (fn->params[i].name == ref.name)
        param = int(i);
    if (param == -2 && sym)
      diags_.error(ref.loc, "a type predicate names a parameter of the function, and '" + ref.name +
                                "' is none");
  }
  const Type *type = ref.args.empty() ? nullptr : resolveType(ref.args[0], /*allowVoid=*/false);
  if (sym && param != -2 && (!type || !type->isError())) {
    sym->predicateParam = param;
    sym->predicateType = type;
    sym->asserts = ref.asserts;
  }
  return ref.asserts ? Type::voidTy() : Type::boolTy();
}

bool Sema::looksBoolean(const Expr *expr) {
  if (isa<BoolLitExpr>(expr))
    return true;
  if (auto *unary = dyn_cast<UnaryExpr>(expr))
    return unary->op == UnaryOp::Not;
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (!bin)
    return false;
  switch (bin->op) {
  case BinaryOp::Eq:
  case BinaryOp::Ne:
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::InstanceOf:
  case BinaryOp::In: return true;
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: return looksBoolean(bin->lhs) && looksBoolean(bin->rhs);
  default: return false;
  }
}

void Sema::beginPredicate(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  bool boolean = !sym->ret || sym->ret->isBool() || sym->asserts;
  if (!boolean || fn->isGenerator || fn->isConstructor || fn->safeTry) {
    predicateBuilds_.push_back({nullptr, fn, Flow(), Flow()});
    return;
  }
  PredicateBuild build{sym, fn, Flow(), Flow()};
  build.whenTrue.unreachable = true;
  build.whenFalse.unreachable = true;
  predicateBuilds_.push_back(std::move(build));
}

void Sema::notePredicateExit(const Flow &whenTrue, const Flow &whenFalse, llvm::SMLoc loc) {
  if (predicateBuilds_.empty() || !predicateBuilds_.back().fn)
    return;
  PredicateBuild &build = predicateBuilds_.back();
  const FuncSymbol *sym = build.fn;
  build.whenTrue = join(build.whenTrue, whenTrue);
  build.whenFalse = join(build.whenFalse, whenFalse);
  // A written predicate: here the parameter must be shown to be T.
  if (sym->predicateParam == -2 || !sym->predicateType || whenTrue.unreachable)
    return;
  const FunctionDecl *decl = build.decl; // the body being checked: maybe a copy
  const VarSymbol *var = sym->predicateParam == -1 ? decl->thisVar
                         : size_t(sym->predicateParam) < decl->params.size()
                             ? decl->params[size_t(sym->predicateParam)].sym
                             : nullptr;
  if (!var)
    return;
  const Type *shown = typeIn(whenTrue, var);
  if (shown->isError() || shown->isNever() || isImplicitlyConvertible(shown, sym->predicateType))
    return;
  std::string what = sym->asserts ? "'asserts " : "'";
  diags_.error(loc, functionLabel(sym->name) + " says " + what + var->name.str() + " is " +
                        sym->predicateType->name().str() + "', but " +
                        (sym->asserts ? "it may end here" : "this may give true") + " while '" +
                        var->name.str() + "' is only known to be '" + shown->name().str() +
                        "'; show it with what narrows types: a union's tag, 'typeof', "
                        "'instanceof', '!= undefined', 'Array.isArray' or a function that tests "
                        "it (checks of fields alone do not make an object of a type)");
}

void Sema::endPredicate(FunctionDecl *fn) {
  PredicateBuild build = std::move(predicateBuilds_.back());
  predicateBuilds_.pop_back();
  if (!build.fn)
    return;
  // Facts about what the function's parameters are, as they were passed: a
  // parameter assigned to in the body is something else by its returns.
  Predicate summary;
  auto position = [&](const VarSymbol *var) -> int {
    if (!var || var->reassigned)
      return -2;
    if (var == fn->thisVar)
      return -1;
    for (size_t i = 0; i < fn->params.size(); ++i)
      if (fn->params[i].sym == var)
        return int(i);
    return -2;
  };
  auto project = [&](const Flow &flow, PredicateSide &side) {
    side.unreachable = flow.unreachable;
    if (flow.unreachable)
      return;
    for (const auto &[var, type] : flow.narrowed)
      if (int i = position(var); i != -2)
        side.narrowed.push_back({i, type});
    for (const VarSymbol *var : flow.defined)
      if (int i = position(var); i != -2)
        side.defined.push_back(i);
    for (const FieldPath &path : flow.definedFields)
      if (int i = position(path.var); i != -2)
        side.fields.push_back({i, path.names});
    for (const Facts::Fact &fact : flow.facts.all()) {
      auto param = [&](const Term &t) { return t.isZero() || position(t.var) != -2; };
      if (param(fact.lhs) && param(fact.rhs))
        side.facts.push_back(fact);
    }
  };
  project(build.whenTrue, summary.whenTrue);
  project(build.whenFalse, summary.whenFalse);
  for (const Param &p : fn->params)
    summary.params.push_back(p.sym);
  summary.self = fn->thisVar;
  predicates_[build.fn] = std::move(summary);
}

const Sema::Predicate *Sema::predicateOf(const FuncSymbol *fn) const {
  if (auto found = predicates_.find(fn); found != predicates_.end())
    return &found->second;
  // Not checked yet (a function called before its body is checked): its
  // summary from a copy of its body, once.
  FunctionDecl *decl = fn->decl;
  if (!decl || decl->checked || !decl->body || decl->isGenerator || decl->isInstance ||
      (fn->ret && !fn->ret->isBool() && !fn->asserts) || !summarizing_.insert(fn).second)
    return nullptr;
  auto *self = const_cast<Sema *>(this);
  BodyState saved = self->enterBody(decl);
  bool muted = diags_.setMuted(true);
  FunctionDecl *copy = self->freshCopy(decl);
  self->checkFunctionBody(copy);
  diags_.setMuted(muted);
  self->leaveBody(saved);
  auto found = predicates_.find(fn);
  return found == predicates_.end() ? nullptr : &found->second;
}

void Sema::applyPredicate(const CallExpr *call, Flow &whenTrue, Flow &whenFalse) const {
  const FuncSymbol *fn = call->func ? call->func : call->method;
  if (!fn || call->virtualCall || !call->unionMethods.empty())
    return;
  // What each parameter stands for at the call.
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto argument = [&](int i) -> const Expr * {
    if (i == -1)
      return call->method && member ? member->object : nullptr;
    return size_t(i) < call->args.size() ? call->args[size_t(i)] : nullptr;
  };
  auto variable = [&](int i) -> const VarSymbol * {
    const Expr *arg = argument(i);
    const VarSymbol *var = arg ? argumentVariable(arg) : nullptr;
    // What it holds now is what was passed, unless the call changed it.
    return var && !changedByCalls(var) ? var : nullptr;
  };
  auto narrowTo = [&](Flow &flow, const VarSymbol *var, const Type *type) {
    const Type *now = typeIn(flow, var);
    if (isImplicitlyConvertible(now, type))
      return; // nothing new
    if (isImplicitlyConvertible(type, now)) {
      narrowVariable(flow, var, type);
      return;
    }
    // The members of what it is now that are also what the parameter was
    // shown to be.
    if (const Type *value = now->nonOptional(); value->isUnion()) {
      std::vector<const Type *> kept;
      for (const Type *m : value->members())
        if (isImplicitlyConvertible(m, type))
          kept.push_back(m);
      narrowVariable(flow, var, kept.empty() ? nullptr : Type::unionOf(kept));
    }
  };

  const Predicate *summary = predicateOf(fn);
  auto apply = [&](const PredicateSide &side, Flow &flow) {
    if (side.unreachable) {
      flow.unreachable = true;
      return;
    }
    for (auto [i, type] : side.narrowed)
      if (const VarSymbol *var = variable(i))
        narrowTo(flow, var, type);
    for (int i : side.defined)
      if (const VarSymbol *var = variable(i); var && var->type->isOptional())
        flow.defined.insert(var);
    for (const auto &[i, names] : side.fields)
      if (const VarSymbol *var = variable(i))
        defineField(flow, FieldPath{var, names});
    // The facts, with each parameter replaced by its argument.
    auto term = [&](const Term &t) -> std::optional<Linear> {
      if (t.isZero())
        return Linear{Term::zero(), 0};
      int i = -2;
      for (size_t k = 0; k < summary->params.size(); ++k)
        if (summary->params[k] == t.var)
          i = int(k);
      if (i == -2)
        return std::nullopt;
      const Expr *arg = argument(i);
      if (!arg)
        return std::nullopt;
      if (t.length) {
        const VarSymbol *var = variable(i);
        if (!var)
          return std::nullopt;
        return Linear{Term::lengthOf(var), 0};
      }
      std::optional<Linear> value = argumentTerm(arg);
      if (value && value->term.var && changedByCalls(value->term.var))
        return std::nullopt;
      return value;
    };
    for (const Facts::Fact &fact : side.facts) {
      std::optional<Linear> l = term(fact.lhs), r = term(fact.rhs);
      if (l && r)
        flow.facts.add(l->term, r->term, fact.offset + r->offset - l->offset);
    }
  };
  if (summary) {
    apply(summary->whenTrue, whenTrue);
    apply(summary->whenFalse, whenFalse);
  }
  // A written predicate says so whatever the body proves; the body is
  // checked to prove it (see notePredicateExit).
  if (fn->predicateParam != -2 && fn->predicateType)
    if (const VarSymbol *var = variable(fn->predicateParam))
      narrowTo(whenTrue, var, fn->predicateType);
}

const Type *Sema::filteredType(const Expr *callback, const Type *element) const {
  const FuncSymbol *fn = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(callback))
    fn = fe->fn->sym;
  else if (auto *name = dyn_cast<NameExpr>(callback))
    fn = name->func                         ? name->func
         : name->var && name->var->function ? name->var->function->sym
                                            : nullptr;
  if (!fn)
    return nullptr;
  const Type *kept = nullptr;
  if (fn->predicateParam == 0 && fn->predicateType && !fn->asserts)
    kept = fn->predicateType;
  if (const Predicate *summary = predicateOf(fn); summary && !kept) {
    const PredicateSide &side = summary->whenTrue;
    if (side.unreachable)
      return nullptr;
    for (auto [i, type] : side.narrowed)
      if (i == 0)
        kept = type;
    if (!kept && element->isOptional() && llvm::is_contained(side.defined, 0))
      kept = element->inner();
  }
  if (!kept || kept == element || !isImplicitlyConvertible(kept, element))
    return nullptr;
  return kept;
}

} // namespace tz
