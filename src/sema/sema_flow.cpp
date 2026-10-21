// Flow-sensitive checking: which `T | undefined` variables hold a value, and
// which facts about ints and lengths hold, at each point of the program.
//
// Facts come from conditions (`i < a.length`), declarations (`let i = 0`) and
// assignments. Anything that may change a variable or an array length makes
// Sema forget the facts about it: an assignment, `pop()`, a call to a user
// function (which may change globals and any array), and, at the head of a
// loop, everything the loop changes anywhere in its body.
//
// Integer overflow is part of the reasoning: int arithmetic wraps, so `i + 1`
// is linear only where Sema can show it does not overflow. Indices and
// positions are computed as long, where that never happens.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();
constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();
// Larger constants are not tracked, which keeps all offsets small.
constexpr uint64_t kMaxConstant = uint64_t(1) << 40;

// The variable `expr` reads, looking through the conversion Sema inserts for
// a narrowed `T | undefined` variable.
const VarSymbol *variableOf(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->operand->type->isOptional())
    expr = conv->operand;
  auto *name = dyn_cast<NameExpr>(expr);
  return name ? name->var : nullptr;
}

const NameExpr *optionalVariable(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  return name && name->var && name->var->type->isOptional() ? name : nullptr;
}

// x < y, or x <= y.
void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual) {
  facts.add(x.term, y.term, y.offset - x.offset + (orEqual ? 1 : 0));
}

bool isArrayLength(const Term &t) { return t.length && t.var->type->nonOptional()->isArray(); }

// Whether a call to a user function may change the value of `t`: a global
// variable, or the length of any array (arrays are shared by reference).
bool changedByCalls(const Term &t) {
  if (!t.var)
    return false;
  if (isArrayLength(t))
    return true;
  return t.var->usedInFunction && !t.var->isConst;
}

} // namespace

Sema::Flow Sema::join(const Flow &a, const Flow &b) {
  if (a.unreachable)
    return b;
  if (b.unreachable)
    return a;
  Flow result;
  result.facts = Facts::meet(a.facts, b.facts);
  for (const VarSymbol *var : a.defined)
    if (b.defined.contains(var))
      result.defined.insert(var);
  return result;
}

void Sema::setDefined(const VarSymbol *var, bool defined) {
  if (defined)
    flow_.defined.insert(var);
  else
    flow_.defined.erase(var);
}

//===----------------------------------------------------------------------===//
// Linear expressions and proofs
//===----------------------------------------------------------------------===//

std::optional<Linear> Sema::linear(const Expr *expr) const {
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    auto *lit = cast<IntLitExpr>(expr);
    if (!expr->type->isInteger() || lit->value > kMaxConstant)
      return std::nullopt;
    return Linear{Term::zero(), static_cast<int64_t>(lit->value)};
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    auto *lit = dyn_cast<IntLitExpr>(unary->operand);
    if (unary->op != UnaryOp::Neg || !lit || !expr->type->isInteger() || lit->value > kMaxConstant)
      return std::nullopt;
    return Linear{Term::zero(), -static_cast<int64_t>(lit->value)};
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(expr)->var;
    if (var && var->type->isInt())
      return Linear{Term::of(var), 0};
    return std::nullopt;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    if (member->name == "length" && expr->type->isInt())
      return lengthOf(member->object);
    return std::nullopt;
  }
  case Expr::Kind::Cast: {
    // int widened to long keeps its value.
    auto *conv = cast<CastExpr>(expr);
    if (conv->isImplicit() && conv->operand->type->isInt() && conv->type->isInteger())
      return linear(conv->operand);
    return std::nullopt;
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    if ((bin->op != BinaryOp::Add && bin->op != BinaryOp::Sub) || !expr->type->isInteger())
      return std::nullopt;
    std::optional<Linear> l = linear(bin->lhs), r = linear(bin->rhs);
    if (!l || !r)
      return std::nullopt;
    Linear result;
    if (r->term.isZero())
      result = {l->term, bin->op == BinaryOp::Add ? l->offset + r->offset : l->offset - r->offset};
    else if (bin->op == BinaryOp::Add && l->term.isZero())
      result = {r->term, l->offset + r->offset};
    else
      return std::nullopt;
    if (result.offset > int64_t(kMaxConstant) || result.offset < -int64_t(kMaxConstant))
      return std::nullopt;
    if (expr->type->isInt()) {
      // Computed in 32 bits: linear only if it provably does not wrap around.
      const Facts &facts = flow_.facts;
      if (result.term.isZero()) {
        if (result.offset < kIntMin || result.offset > kIntMax)
          return std::nullopt;
      } else if (!facts.proves(result.term, Term::zero(), kIntMax - result.offset + 1) ||
                 !facts.proves(Term::zero(), result.term, result.offset - kIntMin + 1)) {
        return std::nullopt;
      }
    }
    return result;
  }
  default:
    return std::nullopt;
  }
}

std::optional<Linear> Sema::lengthOf(const Expr *object) const {
  if (auto *lit = dyn_cast<StringLitExpr>(object))
    return Linear{Term::zero(), static_cast<int64_t>(lit->value.size())};
  if (auto *lit = dyn_cast<ArrayLitExpr>(object)) {
    if (llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; }))
      return std::nullopt;
    return Linear{Term::zero(), static_cast<int64_t>(lit->elements.size())};
  }
  if (const VarSymbol *var = variableOf(object)) {
    const Type *type = var->type->nonOptional();
    if (type->isString() || type->isArray())
      return Linear{Term::lengthOf(var), 0};
  }
  return std::nullopt;
}

bool Sema::provesInBounds(const Expr *object, const Expr *index, bool allowEnd) const {
  std::optional<Linear> at = linear(index), length = lengthOf(object);
  if (!at || !length)
    return false;
  // 0 <= at, that is 0 < at + 1
  if (!flow_.facts.proves(Term::zero(), at->term, at->offset + 1))
    return false;
  // at < length (+ 1 with allowEnd)
  return flow_.facts.proves(at->term, length->term,
                            length->offset - at->offset + (allowEnd ? 1 : 0));
}

bool Sema::provesAtInBounds(const Expr *object, const Expr *index) const {
  std::optional<Linear> at = linear(index), length = lengthOf(object);
  if (at && length && at->term.isZero() && at->offset < 0) {
    // `at(-k)` reads length - k, which needs length >= k.
    return flow_.facts.proves(Term::zero(), length->term, length->offset + at->offset + 1);
  }
  return provesInBounds(object, index, /*allowEnd=*/false);
}

bool Sema::provesNonNegative(const Expr *expr) const {
  std::optional<Linear> value = linear(expr);
  return value && flow_.facts.proves(Term::zero(), value->term, value->offset + 1);
}

bool Sema::provesNotEmpty(const Expr *object) const {
  std::optional<Linear> length = lengthOf(object);
  return length && flow_.facts.proves(Term::zero(), length->term, length->offset);
}

//===----------------------------------------------------------------------===//
// Learning facts
//===----------------------------------------------------------------------===//

void Sema::addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin)
    return;
  BinaryOp op = bin->op;

  if (op == BinaryOp::Eq || op == BinaryOp::Ne) {
    // `x !== undefined`, `x === undefined`
    if (isa<UndefinedExpr>(bin->lhs) || isa<UndefinedExpr>(bin->rhs)) {
      const Expr *side = isa<UndefinedExpr>(bin->rhs) ? bin->lhs : bin->rhs;
      if (const NameExpr *name = optionalVariable(side))
        (op == BinaryOp::Ne ? whenTrue : whenFalse).defined.insert(name->var);
      return;
    }
    // `x === v` where v cannot be undefined: x holds a value if they are equal.
    for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
      auto *conv = dyn_cast<CastExpr>(other);
      const NameExpr *name = optionalVariable(side);
      if (name && conv && conv->isImplicit() && !conv->operand->type->isOptional() &&
          !conv->operand->type->isUndefined())
        (op == BinaryOp::Eq ? whenTrue : whenFalse).defined.insert(name->var);
    }
  }

  switch (op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    break;
  default:
    return;
  }
  std::optional<Linear> l = linear(bin->lhs), r = linear(bin->rhs);
  if (!l || !r)
    return;
  Facts &t = whenTrue.facts, &f = whenFalse.facts;

  // `x !== c` says something when x is already known to be on one side of c:
  // `a.length !== 0` means a.length >= 1.
  if ((op == BinaryOp::Ne || op == BinaryOp::Eq) && (l->term.isZero() || r->term.isZero())) {
    const Linear &x = l->term.isZero() ? *r : *l, &c = l->term.isZero() ? *l : *r;
    Facts &differ = op == BinaryOp::Ne ? t : f;
    if (flow_.facts.proves(c.term, x.term, x.offset - c.offset + 1)) // c <= x
      addLess(differ, c, x, false);
    else if (flow_.facts.proves(x.term, c.term, c.offset - x.offset + 1)) // x <= c
      addLess(differ, x, c, false);
  }
  switch (op) {
  case BinaryOp::Lt:
    addLess(t, *l, *r, false);
    addLess(f, *r, *l, true);
    break;
  case BinaryOp::Le:
    addLess(t, *l, *r, true);
    addLess(f, *r, *l, false);
    break;
  case BinaryOp::Gt:
    addLess(t, *r, *l, false);
    addLess(f, *l, *r, true);
    break;
  case BinaryOp::Ge:
    addLess(t, *r, *l, true);
    addLess(f, *l, *r, false);
    break;
  case BinaryOp::Eq:
    addLess(t, *l, *r, true);
    addLess(t, *r, *l, true);
    break;
  case BinaryOp::Ne:
    addLess(f, *l, *r, true);
    addLess(f, *r, *l, true);
    break;
  default:
    break;
  }
}

std::optional<Linear> Sema::knownLength(const Expr *value) const {
  if (auto *create = dyn_cast<NewExpr>(value)) {
    if (create->args.size() == 1)
      return linear(create->args[0]);
    return Linear{Term::zero(), 0};
  }
  return lengthOf(value);
}

void Sema::assigned(const VarSymbol *var, const Expr *value) {
  const Type *type = var->type;
  if (type->isError())
    return;
  if (type->isOptional())
    setDefined(var, !value->type->isOptional() && !value->type->isUndefined());

  if (type->isInt()) {
    std::optional<Linear> v = linear(value);
    if (v && v->term == Term::of(var)) { // `i = i + k`
      incremented(var, v->offset);
      return;
    }
    flow_.facts.forget(var);
    if (v)
      flow_.facts.addEqual(Term::of(var), v->term, v->offset);
    return;
  }

  flow_.facts.forget(var); // its length
  const Type *base = type->nonOptional();
  if (!base->isString() && !base->isArray())
    return;
  if (value->type->isOptional() || value->type->isUndefined())
    return;
  if (std::optional<Linear> length = knownLength(value))
    if (length->term != Term::lengthOf(var))
      flow_.facts.addEqual(Term::lengthOf(var), length->term, length->offset);
}

void Sema::incremented(const VarSymbol *var, int64_t delta) {
  if (!var->type->isInt())
    return;
  Term t = Term::of(var);
  // Wrapping around would break every fact, so shift them only if it cannot.
  bool safe = delta >= 0 ? flow_.facts.proves(t, Term::zero(), kIntMax - delta + 1)
                         : flow_.facts.proves(Term::zero(), t, delta - kIntMin + 1);
  if (safe)
    flow_.facts.shift(t, delta);
  else
    flow_.facts.forget(var);
}

void Sema::forgetCallEffects(Flow &flow) {
  flow.facts.forgetIf(
      [](const Facts::Fact &f) { return changedByCalls(f.lhs) || changedByCalls(f.rhs); });
  for (const VarSymbol *var : llvm::to_vector(flow.defined))
    if (var->usedInFunction && !var->isConst)
      flow.defined.erase(var);
}

void Sema::calledFunction() { forgetCallEffects(flow_); }

void Sema::arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except) {
  type = type->nonOptional();
  // Arrays of the same type may be the same array under another name.
  auto affected = [&](const Term &t) {
    return t.length && t.var != except && t.var->type->nonOptional() == type;
  };
  flow_.facts.forgetIf([&](const Facts::Fact &f) {
    return (grew && affected(f.lhs)) || (shrank && affected(f.rhs));
  });
}

//===----------------------------------------------------------------------===//
// Loops
//===----------------------------------------------------------------------===//

Sema::LoopScan Sema::scanLoop(llvm::ArrayRef<const Stmt *> stmts,
                              llvm::ArrayRef<const Expr *> exprs) const {
  LoopScan scan;
  for (const Stmt *stmt : stmts)
    if (stmt)
      scanStmt(stmt, scan, 0);
  for (const Expr *expr : exprs)
    if (expr)
      scanExpr(expr, scan, 0);
  return scan;
}

// `depth` counts the loops nested inside the scanned one.
void Sema::scanStmt(const Stmt *stmt, LoopScan &scan, unsigned depth) const {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body)
      scanStmt(s, scan, depth);
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
      if (d.init)
        scanExpr(d.init, scan, depth);
    break;
  case Stmt::Kind::Function:
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
    break;
  case Stmt::Kind::Expr:
    scanExpr(cast<ExprStmt>(stmt)->expr, scan, depth);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    scanExpr(s->cond, scan, depth);
    scanStmt(s->thenStmt, scan, depth);
    if (s->elseStmt)
      scanStmt(s->elseStmt, scan, depth);
    break;
  }
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    scanExpr(s->cond, scan, depth + 1);
    scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    scanStmt(s->body, scan, depth + 1);
    scanExpr(s->cond, scan, depth + 1);
    break;
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    if (s->init)
      scanStmt(s->init, scan, depth + 1);
    if (s->cond)
      scanExpr(s->cond, scan, depth + 1);
    if (s->step)
      scanExpr(s->step, scan, depth + 1);
    scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = cast<ForOfStmt>(stmt);
    scanExpr(s->iterable, scan, depth);
    scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::Return:
    if (auto *value = cast<ReturnStmt>(stmt)->value)
      scanExpr(value, scan, depth);
    break;
  case Stmt::Kind::Break:
    if (depth == 0)
      scan.breaks = true;
    break;
  }
}

void Sema::scanExpr(const Expr *expr, LoopScan &scan, unsigned depth) const {
  auto change = [&](const Expr *target, int64_t delta) {
    auto *name = dyn_cast<NameExpr>(target);
    if (!name)
      return;
    LoopScan::Change &c = scan.changes[name->name];
    c.delta = ++c.count == 1 ? delta : 0;
    c.nested |= depth > 0;
  };
  // A constant step: an int literal from 1 to kMaxLength, or 0.
  auto step = [](const Expr *e) -> int64_t {
    auto *lit = dyn_cast<IntLitExpr>(e);
    return lit && lit->value >= 1 && lit->value <= uint64_t(kMaxLength) ? int64_t(lit->value) : 0;
  };
  auto isName = [](const Expr *e, const Expr *target) {
    auto *a = dyn_cast<NameExpr>(e), *b = dyn_cast<NameExpr>(target);
    return a && b && a->name == b->name;
  };
  auto scanTarget = [&](const Expr *target) {
    if (auto *index = dyn_cast<IndexExpr>(target)) {
      scanExpr(index->object, scan, depth);
      scanExpr(index->index, scan, depth);
      scan.growsArrays = true; // `a[a.length] = x` appends
      scan.releasesElements = true;
    } else if (auto *member = dyn_cast<MemberExpr>(target)) {
      scanExpr(member->object, scan, depth);
      scan.growsArrays = scan.shrinksArrays = true; // `a.length = n`
      scan.releasesElements = true;
    }
  };

  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::Name:
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
      scanExpr(e, scan, depth);
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
      scanExpr(e.expr, scan, depth);
    break;
  case Expr::Kind::Member:
    scanExpr(cast<MemberExpr>(expr)->object, scan, depth);
    break;
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    scanExpr(index->object, scan, depth);
    scanExpr(index->index, scan, depth);
    break;
  }
  case Expr::Kind::Unary:
    scanExpr(cast<UnaryExpr>(expr)->operand, scan, depth);
    break;
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    scanExpr(bin->lhs, scan, depth);
    scanExpr(bin->rhs, scan, depth);
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    scanTarget(assign->target);
    scanExpr(assign->value, scan, depth);
    int64_t delta = 0;
    if (assign->op && (*assign->op == BinaryOp::Add || *assign->op == BinaryOp::Sub)) {
      delta = step(assign->value); // i += k
      if (*assign->op == BinaryOp::Sub)
        delta = -delta;
    } else if (auto *bin = dyn_cast<BinaryExpr>(assign->value); !assign->op && bin) {
      if (bin->op == BinaryOp::Add && isName(bin->lhs, assign->target))
        delta = step(bin->rhs); // i = i + k
      else if (bin->op == BinaryOp::Add && isName(bin->rhs, assign->target))
        delta = step(bin->lhs); // i = k + i
      else if (bin->op == BinaryOp::Sub && isName(bin->lhs, assign->target))
        delta = -step(bin->rhs); // i = i - k
    }
    change(assign->target, delta);
    break;
  }
  case Expr::Kind::Update: {
    auto *update = cast<UpdateExpr>(expr);
    scanTarget(update->target);
    change(update->target, update->increment ? 1 : -1);
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    for (const Expr *arg : call->args)
      scanExpr(arg, scan, depth);
    if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
      if (isGlobalName(member->object, "console"))
        break;
      scanExpr(member->object, scan, depth);
      llvm::StringRef method = member->name;
      if (method == "push" || method == "unshift")
        scan.growsArrays = true;
      else if (method == "pop" || method == "shift")
        scan.shrinksArrays = scan.releasesElements = true;
      else if (method == "splice")
        scan.growsArrays = scan.shrinksArrays = scan.releasesElements = true;
      else if (method == "fill")
        scan.releasesElements = true;
    } else if (!isGlobalName(call->callee, "String")) {
      scan.callsFunctions = true;
    }
    break;
  }
  case Expr::Kind::New:
    for (const Expr *arg : cast<NewExpr>(expr)->args)
      scanExpr(arg, scan, depth);
    break;
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    scanExpr(cond->cond, scan, depth);
    scanExpr(cond->thenExpr, scan, depth);
    scanExpr(cond->elseExpr, scan, depth);
    break;
  }
  case Expr::Kind::Cast:
    scanExpr(cast<CastExpr>(expr)->operand, scan, depth);
    break;
  }
}

std::optional<std::pair<int64_t, int64_t>> Sema::syntacticIntRange(const Expr *expr) const {
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    if (lit->value > uint64_t(kIntMax))
      return std::nullopt;
    auto v = static_cast<int64_t>(lit->value);
    return std::pair{v, v};
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    auto *lit = dyn_cast<IntLitExpr>(unary->operand);
    if (unary->op != UnaryOp::Neg || !lit || lit->value > uint64_t(-kIntMin))
      return std::nullopt;
    auto v = -static_cast<int64_t>(lit->value);
    return std::pair{v, v};
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    const VarSymbol *var = lookup(name->name).var;
    if (var && var->type->isInt())
      return std::pair{kIntMin, kIntMax};
    return std::nullopt;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    if (member->name != "length")
      return std::nullopt;
    if (isa<StringLitExpr>(member->object) || isa<ArrayLitExpr>(member->object))
      return std::pair{int64_t(0), kMaxLength};
    auto *name = dyn_cast<NameExpr>(member->object);
    const VarSymbol *var = name ? lookup(name->name).var : nullptr;
    if (var && (var->type->isString() || var->type->isArray()))
      return std::pair{int64_t(0), kMaxLength};
    return std::nullopt;
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    // `a.length - k`
    auto *lit = dyn_cast<IntLitExpr>(bin->rhs);
    if (bin->op != BinaryOp::Sub || !isa<MemberExpr>(bin->lhs) || !lit || lit->value > kMaxConstant)
      return std::nullopt;
    std::optional<std::pair<int64_t, int64_t>> length = syntacticIntRange(bin->lhs);
    if (!length)
      return std::nullopt;
    auto k = static_cast<int64_t>(lit->value);
    return std::pair{length->first - k, length->second - k};
  }
  return std::nullopt;
}

// Whether `expr` stays <= limit (or >= limit) while the loop runs: by its
// form, or as an int variable the loop does not change, from the facts known
// on entry.
bool Sema::boundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const {
  if (std::optional<std::pair<int64_t, int64_t>> range = syntacticIntRange(expr))
    if (range->second <= limit)
      return true;
  auto *name = dyn_cast<NameExpr>(expr);
  const VarSymbol *var = name ? lookup(name->name).var : nullptr;
  return var && var->type->isInt() && !scan.changes.count(name->name) &&
         flow_.facts.proves(Term::of(var), Term::zero(), limit + 1);
}

bool Sema::boundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const {
  if (std::optional<std::pair<int64_t, int64_t>> range = syntacticIntRange(expr))
    if (range->first >= limit)
      return true;
  auto *name = dyn_cast<NameExpr>(expr);
  const VarSymbol *var = name ? lookup(name->name).var : nullptr;
  return var && var->type->isInt() && !scan.changes.count(name->name) &&
         flow_.facts.proves(Term::zero(), Term::of(var), 1 - limit);
}

// `var` or `var + k` with a constant k >= 0: returns k.
static std::optional<int64_t> varPlus(const Expr *expr, const VarSymbol *var,
                                      llvm::function_ref<const VarSymbol *(llvm::StringRef)> lookupVar) {
  if (auto *name = dyn_cast<NameExpr>(expr))
    return lookupVar(name->name) == var ? std::optional<int64_t>(0) : std::nullopt;
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (!bin || bin->op != BinaryOp::Add)
    return std::nullopt;
  const Expr *other = bin->rhs;
  auto *name = dyn_cast<NameExpr>(bin->lhs);
  if (!name) {
    name = dyn_cast<NameExpr>(bin->rhs);
    other = bin->lhs;
  }
  auto *lit = dyn_cast<IntLitExpr>(other);
  if (!name || !lit || lookupVar(name->name) != var || lit->value > uint64_t(kMaxLength))
    return std::nullopt;
  return static_cast<int64_t>(lit->value);
}

// Whether an `&&`-joined part of `cond` keeps `var + k` low enough while the
// loop runs (`var + k < E` with E small enough) that `var += step` cannot
// wrap around. Returns k.
//
// If var <= INT_MAX - k holds at the head, `var + k` does not wrap, so the
// condition means var <= E - 1 - k, and after the step var <= E - 1 - k +
// step, which stays <= INT_MAX - k when E <= INT_MAX + 1 - step. With `<=`
// the bound is one lower.
std::optional<int64_t> Sema::condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                             const LoopScan &scan, int64_t step) const {
  auto *bin = llvm::dyn_cast_or_null<BinaryExpr>(cond);
  if (!bin)
    return std::nullopt;
  if (bin->op == BinaryOp::LogAnd) {
    if (std::optional<int64_t> k = condBoundsAbove(bin->lhs, var, scan, step))
      return k;
    return condBoundsAbove(bin->rhs, var, scan, step);
  }
  auto lookupVar = [&](llvm::StringRef name) -> const VarSymbol * { return lookup(name).var; };
  const Expr *small = nullptr, *large = nullptr;
  bool strict = false;
  switch (bin->op) {
  case BinaryOp::Lt: small = bin->lhs; large = bin->rhs; strict = true; break;
  case BinaryOp::Gt: small = bin->rhs; large = bin->lhs; strict = true; break;
  case BinaryOp::Le: small = bin->lhs; large = bin->rhs; break;
  case BinaryOp::Ge: small = bin->rhs; large = bin->lhs; break;
  default: return std::nullopt;
  }
  std::optional<int64_t> k = varPlus(small, var, lookupVar);
  if (k && boundedAbove(large, strict ? kIntMax + 1 - step : kIntMax - step, scan))
    return k;
  return std::nullopt;
}

// The same for `var -= step`: `var > E` or `var >= E` keeps var - step at or
// above INT_MIN.
bool Sema::condBoundsBelow(const Expr *cond, const VarSymbol *var, const LoopScan &scan,
                           int64_t step) const {
  auto *bin = llvm::dyn_cast_or_null<BinaryExpr>(cond);
  if (!bin)
    return false;
  if (bin->op == BinaryOp::LogAnd)
    return condBoundsBelow(bin->lhs, var, scan, step) ||
           condBoundsBelow(bin->rhs, var, scan, step);
  auto isVar = [&](const Expr *e) {
    auto *name = dyn_cast<NameExpr>(e);
    return name && lookup(name->name).var == var;
  };
  switch (bin->op) {
  case BinaryOp::Gt: return isVar(bin->lhs) && boundedBelow(bin->rhs, kIntMin + step - 1, scan);
  case BinaryOp::Lt: return isVar(bin->rhs) && boundedBelow(bin->lhs, kIntMin + step - 1, scan);
  case BinaryOp::Ge: return isVar(bin->lhs) && boundedBelow(bin->rhs, kIntMin + step, scan);
  case BinaryOp::Le: return isVar(bin->rhs) && boundedBelow(bin->lhs, kIntMin + step, scan);
  default: return false;
  }
}

Sema::Flow Sema::loopHead(const LoopScan &scan, const Expr *cond, bool induction) const {
  Flow head = flow_;
  head.unreachable = false;
  for (const auto &entry : scan.changes) {
    llvm::StringRef name = entry.first();
    const LoopScan::Change &change = entry.second;
    const VarSymbol *var = lookup(name).var;

    // An induction variable that only grows by a constant step per
    // iteration, while the condition keeps it from wrapping around, keeps its
    // lower bounds (`i >= 0`); one that only shrinks keeps its upper bounds
    // (`i < a.length`).
    //
    // With a condition `i + k < E`, i stays <= INT_MAX - k if it starts so:
    // then `i + k` never wraps around, and the condition keeps i + 1 below
    // that bound. Sema records that invariant too.
    bool keepLower = false, keepUpper = false;
    std::optional<int64_t> invariant;
    if (induction && var && var->type->isInt() && change.count == 1 && !change.nested) {
      if (change.delta > 0) {
        if (std::optional<int64_t> k = condBoundsAbove(cond, var, scan, change.delta)) {
          if (*k == 0) {
            keepLower = true;
          } else if (flow_.facts.proves(Term::of(var), Term::zero(), kIntMax - *k + 1)) {
            keepLower = true;
            invariant = *k;
          }
        }
      }
      keepUpper = change.delta < 0 && condBoundsBelow(cond, var, scan, -change.delta);
    }
    head.facts.forgetIf([&](const Facts::Fact &f) {
      bool lhsHit = f.lhs.var && f.lhs.var->name == name;
      bool rhsHit = f.rhs.var && f.rhs.var->name == name;
      if (!lhsHit && !rhsHit)
        return false;
      if (keepLower && !lhsHit && f.rhs == Term::of(var))
        return false;
      if (keepUpper && !rhsHit && f.lhs == Term::of(var))
        return false;
      return true;
    });
    if (invariant)
      head.facts.add(Term::of(var), Term::zero(), kIntMax - *invariant + 1);
    for (const VarSymbol *defined : llvm::to_vector(head.defined))
      if (defined->name == name)
        head.defined.erase(defined);
  }
  if (scan.callsFunctions)
    forgetCallEffects(head);
  head.facts.forgetIf([&](const Facts::Fact &f) {
    return (scan.growsArrays && f.lhs.length && isArrayLength(f.lhs)) ||
           (scan.shrinksArrays && f.rhs.length && isArrayLength(f.rhs));
  });
  return head;
}

} // namespace tz
