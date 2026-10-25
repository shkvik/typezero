// Literals whose type comes from where the object goes.
//
// An object never converts: an int field and a double field lie differently
// in memory, so `{ x: int }` does not fit where `{ x: double }` is expected.
// Without a type where it is written, a literal would take the types of its
// values, and `const p = { x: 1, y: 2 }; len(p)` with `len(p: Point)` would
// be an error. Instead the code after the declaration is scanned for the
// places the variable goes to: an argument of a function known by name, a
// variable or a field with a type, `push`, a return from a function that
// declares its type. If they agree, the literal is checked as if it were
// written there, `const p: Point = { x: 1, y: 2 }`. If they disagree, the
// literal keeps the types of its values, and the place that does not fit
// reports both (see Sema::coerce).
//
// The scan judges the code by its form, before it is checked: it sees only
// the types of variables and fields it can tell without checking anything,
// and a place it cannot tell leaves the literal as it is.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

struct Sema::DestinationScan {
  llvm::StringRef name; // the variable
  std::vector<std::pair<const Type *, llvm::SMLoc>> found;
  // The names declared in the code scanned so far, which hide the variables
  // of the same name visible at the declaration, with their types where the
  // declaration gives one.
  std::vector<llvm::StringMap<const Type *>> locals;
  // The declared return type of the function whose returns are scanned.
  const Type *returns = nullptr;

  bool hidden(llvm::StringRef other) const {
    return llvm::any_of(
        locals, [&](const llvm::StringMap<const Type *> &scope) { return scope.contains(other); });
  }
  void declare(llvm::StringRef local, const Type *type) {
    if (!locals.empty())
      locals.back()[local] = type;
  }
};

// The values the returns of a function body give, outside the functions
// nested in it.
static void forEachReturn(const Stmt *stmt, llvm::function_ref<void(const Expr *)> each) {
  if (!stmt)
    return;
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
    if (const Expr *value = cast<ReturnStmt>(stmt)->value)
      each(value);
    break;
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body)
      forEachReturn(s, each);
    break;
  case Stmt::Kind::If:
    forEachReturn(cast<IfStmt>(stmt)->thenStmt, each);
    forEachReturn(cast<IfStmt>(stmt)->elseStmt, each);
    break;
  case Stmt::Kind::While:
    forEachReturn(cast<WhileStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::DoWhile:
    forEachReturn(cast<DoWhileStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::For:
    forEachReturn(cast<ForStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::ForOf:
    forEachReturn(cast<ForOfStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::Switch:
    for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases)
      for (const Stmt *s : c.body)
        forEachReturn(s, each);
    break;
  case Stmt::Kind::Labeled:
    forEachReturn(cast<LabeledStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    forEachReturn(s->block, each);
    forEachReturn(s->handler, each);
    forEachReturn(s->finalizer, each);
    break;
  }
  default:
    break;
  }
}

// The type of a parameter without an annotation, from a default value that
// is a literal, as `let x = 5` would give it (sema.cpp).
const Type *literalType(const Expr *value);

// `ok(v)` or `err(e)` as written, before it is checked.
static bool isResultLiteral(const CallExpr *call) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  return name && (name->name == "ok" || name->name == "err") && call->args.size() <= 1 &&
         !call->hasSpread() && !call->optional;
}

bool Sema::literalShaped(const Expr *expr) {
  // An array literal too: `[1, 2]` that goes to a `double[]` holds doubles,
  // and `["Ann", 30]` that goes to `[string, int]` is a tuple.
  if (isa<ObjectLitExpr>(expr) || isa<ArrayLitExpr>(expr))
    return true;
  if (auto *cond = dyn_cast<ConditionalExpr>(expr))
    return literalShaped(cond->thenExpr) || literalShaped(cond->elseExpr);
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    // `ok(1)`: a Result whose other half, and the type of its own, come from
    // where it goes.
    if (isResultLiteral(call))
      return true;
    // `xs.map(x => ({ x, y: 0 }))`
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (!member || (member->name != "map" && member->name != "flatMap") || call->args.size() != 1)
      return false;
    auto *fe = dyn_cast<FunctionExpr>(call->args[0]);
    bool returnsLiteral = false;
    if (fe)
      forEachReturn(fe->fn->body,
                    [&](const Expr *value) { returnsLiteral |= literalShaped(value); });
    return returnsLiteral;
  }
  return false;
}

bool Sema::literalFits(const Expr *expr, const Type *type) const {
  type = type->nonOptional();
  // Into a union: as the member it is written for (see literalMember).
  if (type->isUnion()) {
    if (auto *lit = dyn_cast<ObjectLitExpr>(expr))
      if (const Type *member = literalMember(lit, type))
        return literalFits(expr, member);
    if (const Type *array = type->slotPart(UnionSlot::Array); array && isa<ArrayLitExpr>(expr))
      return literalFits(expr, array);
    return false;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    if (!type->isObject() || type->isTuple() || type->isIncomplete())
      return false;
    std::vector<llvm::StringRef> names;
    for (const ObjectLitExpr::Property &prop : lit->props) {
      if (prop.spread) {
        // The fields of the object spread, if its type is known.
        auto *name = dyn_cast<NameExpr>(prop.value);
        const VarSymbol *var = name ? lookup(name->name).var : nullptr;
        if (!var || !var->type || !var->type->isObject() || var->type->isTuple())
          return false;
        for (const Field &f : var->type->fields())
          if (!llvm::is_contained(prop.omit, f.name))
            names.push_back(f.name);
        continue;
      }
      if (prop.key) // known only once checked
        return false;
      llvm::StringRef name = prop.name;
      const Field *field = type->field(name);
      if (!field || (literalShaped(prop.value) && !literalFits(prop.value, field->type)))
        return false;
      names.push_back(name);
    }
    for (llvm::StringRef name : names)
      if (!type->field(name))
        return false;
    return llvm::all_of(type->fields(), [&](const Field &f) {
      return f.type->isOptional() || llvm::is_contained(names, f.name);
    });
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(expr)) {
    if (type->isTuple()) {
      if (lit->elements.size() != type->fields().size())
        return false;
      for (size_t i = 0; i < lit->elements.size(); ++i) {
        const ArrayLitExpr::Element &e = lit->elements[i];
        if (e.spread || (literalShaped(e.expr) && !literalFits(e.expr, type->fields()[i].type)))
          return false;
      }
      return true;
    }
    if (!type->isArray())
      return false;
    return llvm::all_of(lit->elements, [&](const ArrayLitExpr::Element &e) {
      return e.spread || !literalShaped(e.expr) || literalFits(e.expr, type->inner());
    });
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr))
    return (!literalShaped(cond->thenExpr) || literalFits(cond->thenExpr, type)) &&
           (!literalShaped(cond->elseExpr) || literalFits(cond->elseExpr, type));
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    if (isResultLiteral(call)) {
      if (!type->isResult())
        return false;
      bool isOk = cast<NameExpr>(call->callee)->name == "ok";
      const Type *half = isOk ? type->okType() : type->errType();
      return call->args.empty() || !literalShaped(call->args[0]) ||
             literalFits(call->args[0], half);
    }
    if (!type->isArray())
      return false;
    auto *fe = cast<FunctionExpr>(call->args[0]);
    const Type *element = cast<MemberExpr>(call->callee)->name == "map" ? type->inner() : type;
    bool fits = true;
    forEachReturn(fe->fn->body, [&](const Expr *value) {
      fits &= !literalShaped(value) || literalFits(value, element);
    });
    return fits;
  }
  return true;
}

// A type that fits where the literal goes, without the read-only fields of
// the place: the object itself may still be changed.
static const Type *writable(const Type *type) {
  const Type *object = type->nonOptional();
  if (!object->isObject() ||
      !llvm::any_of(object->fields(), [](const Field &f) { return f.readonly; }))
    return object;
  if (object->isTuple()) {
    std::vector<const Type *> elements;
    for (const Field &f : object->fields())
      elements.push_back(f.type);
    return Type::tuple(elements);
  }
  std::vector<Field> fields(object->fields().begin(), object->fields().end());
  for (Field &f : fields)
    f.readonly = false;
  return Type::object(std::move(fields));
}

const Type *Sema::literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later) {
  DestinationScan scan;
  scan.name = d.name;
  scan.returns = currentFunction_ ? currentFunction_->ret : nullptr;
  scan.locals.emplace_back();
  bool muted = diags_.setMuted(true); // types are resolved again where they are written
  bool visible = true;
  for (const VarDeclarator &next : later) {
    if (next.init)
      scanDestinations(next.init, next.typeAnn ? resolveType(*next.typeAnn, false) : nullptr, scan);
    if (next.name == d.name)
      visible = false;
    scan.declare(next.name, next.typeAnn ? resolveType(*next.typeAnn, false) : nullptr);
  }
  std::vector<const Stmt *> following = std::move(following_);
  following_.clear();
  for (const Stmt *stmt : following) {
    if (!visible && !isa<FunctionDecl>(stmt))
      continue;
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt);
        decl && llvm::any_of(decl->decls, [&](const VarDeclarator &v) { return v.name == d.name; }))
      visible = false;
    scanDestinations(stmt, scan);
  }
  diags_.setMuted(muted);

  // The places the literal fits, and one type that suits them all. A
  // parameter of a generic function takes whatever comes, so it says nothing
  // about the literal.
  std::vector<std::pair<const Type *, llvm::SMLoc>> places;
  for (auto [type, loc] : scan.found)
    if (!type->isError() && !type->isVoid() && !mentionsParam(type) && literalFits(d.init, type))
      places.push_back({type, loc});
  const Type *chosen = nullptr;
  for (auto [type, loc] : places) {
    const Type *candidate = writable(type);
    if (llvm::all_of(places, [&](const std::pair<const Type *, llvm::SMLoc> &other) {
          return isImplicitlyConvertible(candidate, other.first);
        })) {
      chosen = candidate;
      break;
    }
  }
  // Results meet as the returns of a function do: `let r = ok(1)` that is
  // given `err("x")` later is a `Result<int, string>`.
  if (!chosen && llvm::all_of(places, [](const std::pair<const Type *, llvm::SMLoc> &place) {
        return place.first->isResult();
      })) {
    for (auto [type, loc] : places) {
      chosen = chosen ? unify(chosen, type) : type;
      if (!chosen || chosen->isError()) {
        chosen = nullptr;
        break;
      }
    }
  }
  if (!chosen && !places.empty())
    destinationConflict_ = std::move(places);
  return chosen;
}

const Type *Sema::declaredFunctionType(const FunctionDecl *fn) {
  std::vector<const Type *> params;
  for (const Param &param : fn->params) {
    // `Iterable<T>` makes it generic: no one type.
    if (!param.type || isIterableRef(*param.type))
      return nullptr;
    const Type *type = resolveType(*param.type, /*allowVoid=*/false);
    params.push_back(param.optional || param.defaultValue ? Type::optional(type) : type);
  }
  bool hasRest = !fn->params.empty() && fn->params.back().rest;
  const Type *type = Type::function(params, 0, hasRest, Type::voidTy());
  return type->isFunction() ? type : nullptr;
}

bool Sema::scanDestinations(llvm::ArrayRef<Stmt *> stmts, DestinationScan &scan) {
  // A function declared in a block is visible in all of it.
  for (const Stmt *stmt : stmts)
    if (auto *fn = dyn_cast<FunctionDecl>(stmt); fn && fn->name == scan.name)
      return false;
  scan.locals.emplace_back();
  for (const Stmt *stmt : stmts)
    if (auto *fn = dyn_cast<FunctionDecl>(stmt))
      scan.declare(fn->name, declaredFunctionType(fn));
  bool visible = true;
  for (const Stmt *stmt : stmts) {
    scanDestinations(stmt, scan);
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt);
        decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &v) { return v.name == scan.name; })) {
      visible = false;
      break;
    }
  }
  scan.locals.pop_back();
  return visible;
}

void Sema::scanFunctionDestinations(const FunctionDecl *fn, DestinationScan &scan) {
  if (!fn->body || llvm::any_of(fn->params, [&](const Param &p) { return p.name == scan.name; }))
    return;
  const Type *outer = scan.returns;
  scan.returns =
      fn->returnType ? resolveReturnType(const_cast<FunctionDecl *>(fn), nullptr) : nullptr;
  scan.locals.emplace_back();
  for (const Param &param : fn->params) {
    const Type *type = param.type && !isIterableRef(*param.type)
                           ? resolveType(*param.type, /*allowVoid=*/false)
                           : nullptr;
    if (param.defaultValue)
      scanDestinations(param.defaultValue, type, scan);
    scan.declare(param.name, type && param.optional ? Type::optional(type) : type);
  }
  scanDestinations(fn->body->body, scan);
  scan.locals.pop_back();
  scan.returns = outer;
}

void Sema::scanDestinations(const Stmt *stmt, DestinationScan &scan) {
  if (!stmt)
    return;
  auto expr = [&](const Expr *e) {
    if (e)
      scanDestinations(e, nullptr, scan);
  };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    scanDestinations(cast<BlockStmt>(stmt)->body, scan);
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      const Type *type = d.typeAnn ? resolveType(*d.typeAnn, /*allowVoid=*/false) : nullptr;
      if (d.init)
        scanDestinations(d.init, type, scan);
      scan.declare(d.name, type);
    }
    break;
  case Stmt::Kind::Function:
    scanFunctionDestinations(cast<FunctionDecl>(stmt), scan);
    break;
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    expr(s->cond);
    scanDestinations(s->thenStmt, scan);
    scanDestinations(s->elseStmt, scan);
    break;
  }
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(stmt)->cond);
    scanDestinations(cast<WhileStmt>(stmt)->body, scan);
    break;
  case Stmt::Kind::DoWhile:
    scanDestinations(cast<DoWhileStmt>(stmt)->body, scan);
    expr(cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    scan.locals.emplace_back();
    scanDestinations(s->init, scan);
    if (!scan.locals.back().contains(scan.name)) {
      expr(s->cond);
      expr(s->step);
      scanDestinations(s->body, scan);
    }
    scan.locals.pop_back();
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = cast<ForOfStmt>(stmt);
    expr(s->iterable);
    if (s->binding.name == scan.name)
      break;
    scan.locals.emplace_back();
    scan.declare(s->binding.name, nullptr);
    scanDestinations(s->body, scan);
    scan.locals.pop_back();
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    expr(s->discriminant);
    std::vector<Stmt *> all;
    for (const SwitchCase &c : s->cases) {
      expr(c.test);
      all.insert(all.end(), c.body.begin(), c.body.end());
    }
    scanDestinations(all, scan);
    break;
  }
  case Stmt::Kind::Labeled:
    scanDestinations(cast<LabeledStmt>(stmt)->body, scan);
    break;
  case Stmt::Kind::Return:
    if (const Expr *value = cast<ReturnStmt>(stmt)->value)
      scanDestinations(value, scan.returns && !scan.returns->isVoid() ? scan.returns : nullptr,
                       scan);
    break;
  case Stmt::Kind::Throw:
    expr(cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    scanDestinations(s->block, scan);
    if (s->handler && s->param.name != scan.name) {
      scan.locals.emplace_back();
      scan.declare(s->param.name, nullptr);
      scanDestinations(s->handler, scan);
      scan.locals.pop_back();
    }
    scanDestinations(s->finalizer, scan);
    break;
  }
  default:
    break;
  }
}

void Sema::scanDestinations(const Expr *expr, const Type *sink, DestinationScan &scan) {
  auto sub = [&](const Expr *e, const Type *to = nullptr) {
    if (e)
      scanDestinations(e, to, scan);
  };
  if (sink && sink->isError())
    sink = nullptr;
  const Type *object = sink ? sink->nonOptional() : nullptr;
  switch (expr->kind()) {
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    if (sink && name->name == scan.name && !scan.hidden(name->name))
      scan.found.push_back({sink, name->loc()});
    break;
  }
  case Expr::Kind::ObjectLit:
    // `draw({ from: p })`: p goes to the field.
    for (const ObjectLitExpr::Property &prop : cast<ObjectLitExpr>(expr)->props) {
      sub(prop.key);
      const Field *field =
          object && object->isObject() && !prop.spread ? object->field(prop.name) : nullptr;
      sub(prop.value, field ? field->type : nullptr);
    }
    break;
  case Expr::Kind::ArrayLit: {
    auto *lit = cast<ArrayLitExpr>(expr);
    for (size_t i = 0; i < lit->elements.size(); ++i) {
      const ArrayLitExpr::Element &e = lit->elements[i];
      const Type *to = nullptr;
      if (object && object->isArray())
        to = e.spread ? object : object->inner();
      else if (object && object->isTuple() && !e.spread && i < object->fields().size())
        to = object->fields()[i].type;
      sub(e.expr, to);
    }
    break;
  }
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    sub(cond->cond);
    sub(cond->thenExpr, sink);
    sub(cond->elseExpr, sink);
    break;
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    bool passes =
        bin->op == BinaryOp::Nullish || bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
    sub(bin->lhs, bin->op == BinaryOp::Nullish ? sink : nullptr);
    sub(bin->rhs, passes ? sink : nullptr);
    break;
  }
  case Expr::Kind::Sequence: {
    auto *seq = cast<SequenceExpr>(expr);
    for (size_t i = 0; i < seq->exprs.size(); ++i)
      sub(seq->exprs[i], i + 1 == seq->exprs.size() ? sink : nullptr);
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    auto *target = dyn_cast<NameExpr>(assign->target);
    if (target && target->name == scan.name && !scan.hidden(target->name)) {
      // `p = q`: p must hold what q holds.
      if (const Type *type = assign->op ? nullptr : peekType(assign->value, scan))
        scan.found.push_back({type, assign->loc()});
      sub(assign->value);
      break;
    }
    if (!isa<NameExpr>(assign->target))
      sub(assign->target);
    sub(assign->value, assign->op ? nullptr : peekType(assign->target, scan));
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    std::vector<const Type *> params = calleeParams(call, scan);
    sub(call->callee);
    for (size_t i = 0; i < call->args.size(); ++i)
      sub(call->args[i], i < params.size() && !call->spreads(i) ? params[i] : nullptr);
    break;
  }
  case Expr::Kind::Function:
    scanFunctionDestinations(cast<FunctionExpr>(expr)->fn, scan);
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
      sub(e);
    break;
  case Expr::Kind::Member:
    sub(cast<MemberExpr>(expr)->object);
    break;
  case Expr::Kind::Index:
    sub(cast<IndexExpr>(expr)->object);
    sub(cast<IndexExpr>(expr)->index);
    break;
  case Expr::Kind::Unary:
    sub(cast<UnaryExpr>(expr)->operand);
    break;
  case Expr::Kind::Update:
    sub(cast<UpdateExpr>(expr)->target);
    break;
  case Expr::Kind::New: {
    // The arguments of the constructor of a class, if it has one variant.
    auto *create = cast<NewExpr>(expr);
    const FuncSymbol *ctor = nullptr;
    if (ClassDecl *cls = scan.hidden(create->name) ? nullptr : lookup(create->name).cls)
      if (cls->constructors.size() == 1 && cls->constructors[0]->sym &&
          !cls->constructors[0]->sym->pendingParams)
        ctor = cls->constructors[0]->sym;
    for (size_t i = 0; i < create->args.size(); ++i)
      sub(create->args[i], ctor && i < ctor->params.size() && !ctor->hasRest ? ctor->params[i]
                                                                              : nullptr);
    break;
  }
  case Expr::Kind::Cast:
    sub(cast<CastExpr>(expr)->operand);
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain);
    break;
  case Expr::Kind::Yield:
    if (const Expr *value = cast<YieldExpr>(expr)->value)
      sub(value);
    break;
  case Expr::Kind::Await:
    sub(cast<AwaitExpr>(expr)->value);
    break;
  default:
    break;
  }
}

const Type *Sema::peekName(llvm::StringRef name, const DestinationScan &scan) const {
  for (const llvm::StringMap<const Type *> &scope : llvm::reverse(scan.locals))
    if (auto found = scope.find(name); found != scope.end())
      return found->second;
  VarSymbol *var = lookup(name).var;
  return var ? var->type : nullptr;
}

const Type *Sema::peekType(const Expr *expr, const DestinationScan &scan) const {
  if (auto *name = dyn_cast<NameExpr>(expr))
    return name->name == scan.name ? nullptr : peekName(name->name, scan);
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    const Type *object = peekType(member->object, scan);
    object = object ? object->nonOptional() : nullptr;
    if (object && object->isRecord()) // `r.key`
      return object->valueType();
    const Field *field = object && object->isObject() ? object->field(member->name) : nullptr;
    return field ? field->type : nullptr;
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    const Type *object = peekType(index->object, scan);
    if (object && object->isRecord())
      return object->valueType();
    return object && object->isArray() ? object->inner() : nullptr;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr); cond && literalShaped(cond)) {
    const Type *a = peekType(cond->thenExpr, scan), *b = peekType(cond->elseExpr, scan);
    const Type *common = a && b ? unify(a, b) : nullptr;
    return common && !common->isError() ? common : nullptr;
  }
  // `err("x")`, whose other half can be anything.
  if (auto *call = dyn_cast<CallExpr>(expr); call && isResultLiteral(call)) {
    const Type *half = Type::voidTy();
    if (!call->args.empty()) {
      half = peekType(call->args[0], scan);
      if (!half)
        half = literalType(call->args[0]);
    }
    if (!half)
      return nullptr;
    return cast<NameExpr>(call->callee)->name == "ok" ? Type::result(half, Type::never())
                                                      : Type::result(Type::never(), half);
  }
  return nullptr;
}

std::vector<const Type *> Sema::calleeParams(const CallExpr *call, const DestinationScan &scan) {
  // The parameters of a function type, the elements of a rest parameter
  // repeated for the arguments it takes.
  auto spread = [&](llvm::ArrayRef<const Type *> params, bool hasRest) {
    std::vector<const Type *> types(params.begin(), params.end());
    if (hasRest && !types.empty()) {
      const Type *rest = types.back();
      types.pop_back();
      while (types.size() < call->args.size())
        types.push_back(rest && rest->isArray() ? rest->inner() : nullptr);
    }
    return types;
  };
  if (auto *name = dyn_cast<NameExpr>(call->callee)) {
    if (scan.hidden(name->name)) {
      const Type *type = peekName(name->name, scan);
      return type && type->isFunction() ? spread(type->params(), type->hasRest())
                                        : std::vector<const Type *>{};
    }
    ScopeEntry entry = lookup(name->name);
    FuncSymbol *func = entry.func;
    if (entry.var && entry.var->function)
      func = entry.var->function->sym;
    if (func && !func->pendingParams)
      return spread(func->params, func->hasRest);
    if (entry.var && entry.var->type && entry.var->type->isFunction())
      return spread(entry.var->type->params(), entry.var->type->hasRest());
    return {};
  }
  if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
    // A method of a class, or a static one: if it has one variant.
    auto variant = [&](const ClassDecl *cls, bool isStatic) -> std::vector<const Type *> {
      std::vector<FunctionDecl *> methods = methodsNamed(cls, member->name, isStatic);
      if (methods.size() != 1 || !methods[0]->sym || methods[0]->sym->pendingParams)
        return {};
      return spread(methods[0]->sym->params, methods[0]->sym->hasRest);
    };
    if (auto *name = dyn_cast<NameExpr>(member->object); name && !scan.hidden(name->name))
      if (ClassDecl *cls = lookup(name->name).cls)
        return variant(cls, /*isStatic=*/true);
    const Type *object = peekType(member->object, scan);
    if (!object)
      return {};
    if (ClassDecl *cls = classOf(object->nonOptional()))
      return variant(cls, /*isStatic=*/false);
    object = object->nonOptional();
    if (object->isArray()) {
      // Where the arguments of array methods go.
      static const char *const elements[] = {"push", "unshift", "includes", "indexOf",
                                             "lastIndexOf"};
      if (llvm::is_contained(elements, member->name))
        return std::vector<const Type *>(call->args.size(), object->inner());
      if (member->name == "fill")
        return {object->inner()};
      return {};
    }
    if (object->isObject())
      if (const Field *field = object->field(member->name); field && field->type->isFunction())
        return spread(field->type->params(), field->type->hasRest());
    // Where the keys and values of a Map and a Set go.
    if (object->isMap() && member->name == "set")
      return {object->keyType(), object->valueType()};
    if (object->isDict() && !object->isRecord() &&
        (member->name == "get" || member->name == "has" || member->name == "delete" ||
         member->name == "add"))
      return {object->keyType()};
  }
  return {};
}

} // namespace tz
