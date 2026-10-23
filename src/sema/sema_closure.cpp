// Closures: arrows, function expressions and functions declared inside
// blocks, the variables they capture, calls through function values, and the
// methods of arrays that take callbacks.
//
// A closure captures the variables of the functions around it that its body
// uses (noteUse). CodeGen copies the value of a captured variable into the
// closure's environment if the variable never changes after the closure is
// created; any other captured variable lives in a box that the function
// declaring it and the closures share (VarSymbol::boxed).

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

void visitExprs(const Expr *expr, llvm::function_ref<void(const Expr *)> visit) {
  visit(expr);
  auto sub = [&](const Expr *e) { visitExprs(e, visit); };
  switch (expr->kind()) {
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
      sub(e);
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
      sub(e.expr);
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props)
      sub(p.value);
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
  case Expr::Kind::Binary:
    sub(cast<BinaryExpr>(expr)->lhs);
    sub(cast<BinaryExpr>(expr)->rhs);
    break;
  case Expr::Kind::Assign:
    sub(cast<AssignExpr>(expr)->target);
    sub(cast<AssignExpr>(expr)->value);
    break;
  case Expr::Kind::Update:
    sub(cast<UpdateExpr>(expr)->target);
    break;
  case Expr::Kind::Call:
    sub(cast<CallExpr>(expr)->callee);
    for (const Expr *arg : cast<CallExpr>(expr)->args)
      sub(arg);
    break;
  case Expr::Kind::New:
    for (const Expr *arg : cast<NewExpr>(expr)->args)
      sub(arg);
    break;
  case Expr::Kind::Conditional:
    sub(cast<ConditionalExpr>(expr)->cond);
    sub(cast<ConditionalExpr>(expr)->thenExpr);
    sub(cast<ConditionalExpr>(expr)->elseExpr);
    break;
  case Expr::Kind::Cast:
    sub(cast<CastExpr>(expr)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs)
      sub(e);
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain);
    break;
  case Expr::Kind::Function:
    visitExprs(cast<FunctionExpr>(expr)->fn, visit);
    break;
  default:
    break;
  }
}

void visitExprs(const Stmt *stmt, llvm::function_ref<void(const Expr *)> visit) {
  if (!stmt)
    return;
  auto expr = [&](const Expr *e) {
    if (e)
      visitExprs(e, visit);
  };
  auto sub = [&](const Stmt *s) { visitExprs(s, visit); };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body)
      sub(s);
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
      expr(d.init);
    break;
  case Stmt::Kind::Function: {
    auto *fn = cast<FunctionDecl>(stmt);
    for (const Param &param : fn->params)
      expr(param.defaultValue);
    sub(fn->body);
    break;
  }
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    expr(s->cond);
    sub(s->thenStmt);
    sub(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(stmt)->cond);
    sub(cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    sub(cast<DoWhileStmt>(stmt)->body);
    expr(cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    sub(s->init);
    expr(s->cond);
    expr(s->step);
    sub(s->body);
    break;
  }
  case Stmt::Kind::ForOf:
    expr(cast<ForOfStmt>(stmt)->iterable);
    sub(cast<ForOfStmt>(stmt)->body);
    break;
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    expr(s->discriminant);
    for (const SwitchCase &c : s->cases) {
      expr(c.test);
      for (const Stmt *body : c.body)
        sub(body);
    }
    break;
  }
  case Stmt::Kind::Labeled:
    sub(cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    expr(cast<ReturnStmt>(stmt)->value);
    break;
  default:
    break;
  }
}

namespace {

// The names `stmt` assigns to: `x = v`, `x += v`, `x++`...
void collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names) {
  visitExprs(stmt, [&](const Expr *e) {
    const Expr *target = nullptr;
    if (auto *assign = dyn_cast<AssignExpr>(e))
      target = assign->target;
    else if (auto *update = dyn_cast<UpdateExpr>(e))
      target = update->target;
    if (auto *name = llvm::dyn_cast_or_null<NameExpr>(target))
      if (!llvm::is_contained(names, name->name))
        names.push_back(name->name);
  });
}

// How a callback of a built-in method is called, for messages.
const char *callbackArgs(llvm::StringRef method) {
  if (method == "reduce" || method == "reduceRight")
    return "(accumulator, value, index, array)";
  if (method == "sort")
    return "(a, b)";
  return "(value, index, array)";
}

const char *ordinal(size_t i) {
  static const char *const names[] = {"first", "second", "third", "fourth", "fifth"};
  return i < 5 ? names[i] : "next";
}

// What the arguments of a callback are, by position.
const char *callbackArgName(llvm::StringRef method, size_t i) {
  bool reduce = method == "reduce" || method == "reduceRight";
  if (reduce)
    return i == 0 ? "accumulator" : i == 1 ? "value" : i == 2 ? "index" : "array";
  return i == 0 ? "value" : i == 1 ? "index" : "array";
}

} // namespace

//===----------------------------------------------------------------------===//
// Function expressions
//===----------------------------------------------------------------------===//

Sema::Signature Sema::signatureOf(const Type *fnType) {
  Signature sig;
  sig.params.assign(fnType->params().begin(), fnType->params().end());
  sig.minArgs = fnType->minArgs();
  sig.ret = fnType->ret();
  // As in TypeScript, a function whose result is dropped may return anything.
  sig.voidResult = fnType->ret()->isVoid();
  return sig;
}

// The type of a parameter without an annotation, from a default value that
// is a literal, as `let x = 5` would give it.
const Type *literalType(const Expr *value);

Expr *Sema::checkFunctionExpr(FunctionExpr *fe, const Signature *sig) {
  FunctionDecl *fn = fe->fn;
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->parent = currentDecl();
  fn->creationSeq = ++seq_;

  // Parameters: an annotation, or the type the context passes.
  size_t named = fn->params.size();
  bool afterOptional = false;
  for (size_t i = 0; i < named; ++i) {
    Param &param = fn->params[i];
    const Type *given = sig && i < sig->params.size() ? sig->params[i] : nullptr;
    bool mayOmit = param.optional || param.defaultValue;
    const Type *type;   // inside the body
    const Type *passed; // by callers
    if (param.type) {
      type = resolveType(*param.type, /*allowVoid=*/false);
      if (param.optional)
        type = Type::optional(type);
      passed = mayOmit ? Type::optional(type) : type;
      if (given && passed != given && !passed->isError() && !given->isError() && !param.rest) {
        diags_.error(param.loc, "parameter '" + param.name + "' is declared as '" + passed->name() +
                                    "', but the function gets '" + given->name() + "' here");
        passed = given; // reported once
        type = param.defaultValue ? given->nonOptional() : given;
      }
    } else if (given) {
      passed = given;
      type = param.defaultValue ? given->nonOptional() : given;
      if (param.defaultValue && !given->isOptional())
        diags_.error(param.defaultValue->loc(), "the default value of '" + param.name +
                                                    "' is never used: the function always gets a value here");
    } else if (param.defaultValue && (type = literalType(param.defaultValue))) {
      passed = Type::optional(type);
    } else {
      diags_.error(param.loc, "parameter '" + param.name +
                                  "' needs a type: nothing here says what the function is called with");
      type = passed = Type::error();
    }
    if (param.rest) {
      if (!type->isArray() && !type->isError())
        diags_.error(param.loc, "a rest parameter must be an array, e.g. '..." + param.name + ": int[]'");
      sym->hasRest = true;
      passed = type;
    } else if (!mayOmit) {
      if (afterOptional)
        diags_.error(param.loc, "a required parameter cannot follow an optional one");
      sym->minArgs = i + 1;
    }
    afterOptional |= mayOmit;
    param.declaredType = type;
    sym->params.push_back(passed);
  }
  // It takes the arguments it does not name too, so that its type is the one
  // expected.
  if (sig && sig->pad) {
    for (size_t i = named; i < sig->params.size(); ++i) {
      Param hidden;
      hidden.name = ctx_.hiddenName();
      hidden.loc = fe->loc();
      hidden.declaredType = sig->params[i];
      fn->params.push_back(hidden);
      sym->params.push_back(sig->params[i]);
    }
    sym->minArgs = sig->minArgs;
  }

  if (sig && sig->voidResult) {
    sym->ret = Type::voidTy();
    sym->discardsResult = true;
  } else if (fn->returnType) {
    sym->ret = resolveType(*fn->returnType, /*allowVoid=*/true);
  } else if (sig && sig->ret) {
    sym->ret = sig->ret;
  } else if (!returnsValue(fn->body)) {
    sym->ret = Type::voidTy();
  }

  // A function that calls itself, through its constant or its own name,
  // needs its return type before its body is checked: found on copies, as
  // for recursive declarations (see inferCycle).
  if (!sym->ret) {
    llvm::StringSet<> names;
    collectNames(fn->body, names);
    bool recursive = !fn->ownName.empty() && names.contains(fn->ownName);
    for (const auto &entry : names) {
      VarSymbol *var = lookup(entry.getKey()).var;
      recursive |= var && var->lambda == fn;
    }
    if (recursive) {
      cycleOf_[sym] = static_cast<unsigned>(cycles_.size());
      cycles_.push_back({sym});
      BodyState saved = enterBody(fn);
      bool ownScope = !fn->ownName.empty();
      if (ownScope)
        declareOwnName(fn);
      inferCycle(cycles_.back());
      if (ownScope)
        popScope();
      leaveBody(saved);
    }
  }
  checkClosureBody(fn);
  fe->type = functionType(sym);
  return fe;
}

void Sema::declareOwnName(FunctionDecl *fn) {
  pushScope();
  VarSymbol *self = ctx_.newVar();
  self->name = fn->ownName;
  self->loc = fn->loc();
  self->isConst = true;
  self->lambda = fn;
  self->owner = fn;
  self->declSeq = ++seq_;
  declare(fn->ownName, fn->loc(), {self, nullptr});
}

void Sema::checkClosureBody(FunctionDecl *fn) {
  BodyState saved = enterBody(fn);
  // `function f() { ... f() ... }`: the name is visible inside.
  bool ownScope = fn->isExpression && !fn->ownName.empty();
  if (ownScope)
    declareOwnName(fn);
  if (fn->sym->ret)
    checkFunctionBody(fn);
  else
    checkAndInfer(fn);
  if (ownScope)
    popScope();
  leaveBody(saved);
}

//===----------------------------------------------------------------------===//
// Functions declared in blocks
//===----------------------------------------------------------------------===//

void Sema::checkStatements(BlockStmt *block) {
  declareTypes(block->body);
  NestedGroup *group = declareNested(block);
  if (group)
    groups_.push_back(group);
  BlockNames names{functions_.size(), {}};
  for (Stmt *stmt : block->body)
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt))
      for (const VarDeclarator &d : decl->decls)
        names.names.push_back(d.name);
  blockNames_.push_back(std::move(names));
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse) {
      // The functions are created here.
      group->creationSeq = ++seq_;
      for (FunctionDecl *fn : group->members)
        fn->var->declSeq = group->creationSeq;
    }
    checkStmt(block->body[i]);
  }
  blockNames_.pop_back();
  if (group) {
    finishGroup(group);
    groups_.pop_back();
  }
}

NestedGroup *Sema::declareNested(BlockStmt *block) {
  std::vector<FunctionDecl *> members;
  for (Stmt *stmt : block->body)
    if (auto *fn = dyn_cast<FunctionDecl>(stmt))
      members.push_back(fn);
  if (members.empty())
    return nullptr;
  NestedGroup *group = ctx_.newGroup();
  block->group = group;
  group->owner = currentDecl();
  group->scopeDepth = scopes_.size();
  group->functionDepth = functions_.size();
  llvm::StringSet<> names;
  for (FunctionDecl *fn : members) {
    fn->group = group;
    fn->parent = currentDecl();
    group->members.push_back(fn);
    names.insert(fn->name);
    VarSymbol *var = ctx_.newVar();
    var->name = fn->name;
    var->loc = fn->loc();
    var->isConst = true;
    var->owner = currentDecl();
    var->function = fn;
    fn->var = var;
    declareFunction(fn, var);
    collectAssigned(fn->body, group->assignedNames);
  }
  for (Stmt *stmt : block->body)
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt))
      for (const VarDeclarator &d : decl->decls)
        group->declaredNames.push_back(d.name);
  // The functions are created before the first statement that mentions
  // them, so that they can copy the variables declared before it.
  for (size_t i = 0; i < block->body.size() && group->firstUse == ~size_t(0); ++i) {
    if (isa<FunctionDecl>(block->body[i]))
      continue;
    llvm::StringSet<> used;
    collectNames(block->body[i], used);
    if (llvm::any_of(used, [&](const auto &entry) { return names.contains(entry.getKey()); }))
      group->firstUse = i;
  }
  findCycles(group->members);
  return group;
}

void Sema::finishGroup(NestedGroup *group) {
  // Every variable of the block is declared now.
  group->finished = true;
  for (FunctionDecl *fn : group->members) {
    if (!fn->sym->ret)
      inferReturnType(fn->sym);
    if (fn->checked)
      continue;
    BodyState saved = enterBody(fn);
    checkFunctionBody(fn);
    leaveBody(saved);
  }

  // A function the block calls (or passes on) before a variable it uses is
  // declared: JavaScript would throw a ReferenceError when it reads it.
  auto declaredAfter = [&](const NestedGroup::Use &use) -> VarSymbol * {
    std::vector<FunctionDecl *> reached{use.fn};
    for (size_t i = 0; i < reached.size(); ++i)
      for (FunctionDecl *sibling : reached[i]->siblings)
        if (!llvm::is_contained(reached, sibling))
          reached.push_back(sibling);
    for (FunctionDecl *fn : reached)
      for (VarSymbol *var : fn->captures)
        if (var->owner == group->owner && var->declSeq > use.seq)
          return var;
    return nullptr;
  };
  for (const NestedGroup::Use &use : group->uses) {
    if (VarSymbol *var = declaredAfter(use)) {
      diags_.error(use.loc, "'" + use.fn->name + "' uses '" + var->name +
                                "' before it is declared: JavaScript would throw a ReferenceError when it "
                                "runs here; move the declaration of '" + var->name + "' up");
      diags_.note(var->loc, "'" + var->name + "' is declared here");
    }
  }
}

void Sema::noteUse(VarSymbol *var, llvm::SMLoc loc) {
  if (var->isGlobal)
    return;
  // The block's own code uses one of its functions: it runs, or may run,
  // from here (see finishGroup).
  if (FunctionDecl *member = var->function; member && currentDecl() == member->group->owner)
    member->group->uses.push_back({member, seq_, loc});
  for (size_t i = functions_.size(); i-- > 0;) {
    FunctionDecl *fn = functions_[i];
    // Its own variables, and the function itself through its name.
    if (fn == var->owner || fn == var->lambda)
      break;
    // The functions of a group share their environment: they reach each
    // other through it.
    if (var->function && fn->group && fn->group == var->function->group) {
      if (fn != var->function && !llvm::is_contained(fn->siblings, var->function))
        fn->siblings.push_back(var->function);
      break;
    }
    if (!llvm::is_contained(fn->captures, var))
      fn->captures.push_back(var);
    if (fn->group && !llvm::is_contained(fn->group->captures, var))
      fn->group->captures.push_back(var);
    var->captured = true;
    unsigned created = fn->group ? fn->group->creationSeq : fn->creationSeq;
    if (created <= var->declSeq)
      var->capturedEarly = true;
  }
}

//===----------------------------------------------------------------------===//
// Functions as values
//===----------------------------------------------------------------------===//

FunctionDecl *Sema::namedFunction(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name)
    return nullptr;
  if (name->func)
    return name->func->decl;
  if (VarSymbol *var = name->var)
    return var->function ? var->function : var->isConst ? var->lambda : nullptr;
  return nullptr;
}

Expr *Sema::adaptFunction(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  if (!namedFunction(expr) || from->hasRest() || to->hasRest())
    return nullptr;
  llvm::ArrayRef<const Type *> params = from->params(), wanted = to->params();
  for (size_t i = 0; i < wanted.size() && i < params.size(); ++i)
    if (params[i] != wanted[i] && !(params[i]->isOptional() && params[i]->inner() == wanted[i]))
      return nullptr;
  // The parameters callers do not pass must be ones it can go without.
  if (wanted.size() < from->minArgs())
    return nullptr;
  if (!to->ret()->isVoid() && from->ret() != to->ret())
    return nullptr;
  auto *cast = ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;
  return cast;
}

FunctionExpr *Sema::builtinFunctionValue(Expr *arg, const Signature &expected, unsigned primary,
                                          llvm::StringRef method) {
  constexpr unsigned kAny = ~0u;
  llvm::StringRef name;
  unsigned maxArgs = 0;
  bool returnsNothing = false;
  if (auto *n = dyn_cast<NameExpr>(arg)) {
    static const std::pair<llvm::StringLiteral, unsigned> globals[] = {
        {"String", 1}, {"Number", 1}, {"parseInt", 2}, {"parseFloat", 1}, {"isNaN", 1}, {"isFinite", 1},
        {"Boolean", 1}};
    for (const auto &[global, count] : globals)
      if (isGlobalName(n, global)) {
        name = global;
        maxArgs = count;
      }
  } else if (auto *m = dyn_cast<MemberExpr>(arg)) {
    name = m->name;
    if (isGlobalName(m->object, "Math")) {
      static const llvm::StringLiteral two[] = {"pow", "atan2", "imul"};
      static const llvm::StringLiteral any[] = {"min", "max", "hypot"};
      static const llvm::StringLiteral one[] = {
          "floor", "ceil", "round", "trunc", "abs",  "sign", "sqrt",  "cbrt",  "exp",   "expm1",
          "log",   "log1p", "log2", "log10", "sin",  "cos",  "tan",   "asin",  "acos",  "atan",
          "sinh",  "cosh", "tanh",  "asinh", "acosh", "atanh", "fround", "clz32"};
      if (llvm::is_contained(two, name))
        maxArgs = 2;
      else if (llvm::is_contained(any, name))
        maxArgs = kAny;
      else if (llvm::is_contained(one, name))
        maxArgs = 1;
      else if (name != "random")
        name = {};
    } else if (isGlobalName(m->object, "Number")) {
      if (name == "parseInt")
        maxArgs = 2;
      else if (name == "parseFloat" || name == "isNaN" || name == "isFinite" || name == "isInteger" ||
               name == "isSafeInteger")
        maxArgs = 1;
      else
        name = {};
    } else if (isGlobalName(m->object, "String")) {
      if (name == "fromCharCode" || name == "fromCodePoint")
        maxArgs = kAny;
      else
        name = {};
    } else if (isGlobalName(m->object, "console") && name == "log") {
      maxArgs = kAny;
      returnsNothing = true;
    } else {
      name = {};
    }
  }
  if (name.empty())
    return nullptr;

  size_t count = std::min<size_t>(maxArgs, expected.params.size());
  if (count > primary) {
    // `map(parseInt)`: parseInt would take the index as its radix.
    std::string qualified = name.str();
    if (auto *m = dyn_cast<MemberExpr>(arg))
      qualified = cast<NameExpr>(m->object)->name.str() + "." + qualified;
    diags_.error(arg->loc(), "'" + qualified + "' would also get the " + callbackArgName(method, primary) +
                                 " as its " + ordinal(primary) + " argument: '" + method +
                                 "' calls it with " + callbackArgs(method) + "; pass only what it needs: 'x => " +
                                 qualified + "(x)'");
    count = primary;
  }
  auto *fn = ctx_.make<FunctionDecl>(arg->loc(), name);
  fn->isExpression = fn->isArrow = fn->concise = true;
  std::vector<Expr *> args;
  for (size_t i = 0; i < count; ++i) {
    Param param;
    param.name = ctx_.hiddenName();
    param.loc = arg->loc();
    fn->params.push_back(param);
    args.push_back(ctx_.make<NameExpr>(arg->loc(), param.name));
  }
  auto *call = ctx_.make<CallExpr>(arg->loc(), arg, std::move(args));
  fn->body = ctx_.make<BlockStmt>(arg->loc());
  if (returnsNothing)
    fn->body->body.push_back(ctx_.make<ExprStmt>(arg->loc(), call));
  else
    fn->body->body.push_back(ctx_.make<ReturnStmt>(arg->loc(), call));
  return ctx_.make<FunctionExpr>(arg->loc(), fn);
}

Expr *Sema::checkFunctionArg(Expr *arg, const Signature &expected, unsigned primary, llvm::StringRef method) {
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    // The method only calls it, during the call.
    fe->fn->nonEscaping = true;
    return checkFunctionExpr(fe, &expected);
  }
  if (FunctionExpr *wrapper = builtinFunctionValue(arg, expected, primary, method)) {
    wrapper->fn->nonEscaping = true;
    return checkFunctionExpr(wrapper, &expected);
  }
  arg = checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError())
    return arg;
  if (!type->isFunction()) {
    diags_.error(arg->loc(), "'" + method + "' needs a function, found '" + type->name() + "'");
    return arg;
  }
  FunctionDecl *known = namedFunction(arg);
  std::string name = known && !known->name.empty() ? "'" + known->name.str() + "'" : "the function";
  if (type->hasRest()) {
    diags_.error(arg->loc(), name + " takes a rest parameter; pass an arrow that calls it instead");
    return arg;
  }
  llvm::ArrayRef<const Type *> params = type->params();
  size_t passed = std::min(params.size(), expected.params.size());
  for (size_t i = 0; i < passed; ++i) {
    const Type *param = params[i], *given = expected.params[i];
    if (param != given && !(param->isOptional() && param->inner() == given)) {
      diags_.error(arg->loc(), name + " takes '" + param->name() + "' as its " + ordinal(i) + " argument, but '" +
                                   method + "' passes '" + given->name() + "'");
      return arg;
    }
    // `map(parseInt)`: the index would land in an optional parameter, which
    // the function surely meant for something else.
    if (i >= primary && i >= type->minArgs()) {
      std::string paramName = known && i < known->params.size() ? " '" + known->params[i].name.str() + "'" : "";
      diags_.error(arg->loc(), name + " would also get the " + callbackArgName(method, i) +
                                   " in its optional parameter" + paramName + ": '" + method + "' calls it with " +
                                   callbackArgs(method) + "; pass an arrow that gives it only what it needs");
      return arg;
    }
  }
  if (type->minArgs() > expected.params.size()) {
    diags_.error(arg->loc(), "'" + method + "' passes " + argumentCount(unsigned(expected.params.size()),
                                                                         unsigned(expected.params.size())) +
                                 ", but " + name + " needs " + std::to_string(type->minArgs()));
    return arg;
  }
  if (expected.ret && type->ret() != expected.ret && !expected.voidResult) {
    diags_.error(arg->loc(), "'" + method + "' needs a function that returns '" + expected.ret->name() +
                                 "', but " + name + " returns '" + type->ret()->name() + "'");
    return arg;
  }
  return arg;
}

//===----------------------------------------------------------------------===//
// Methods of arrays that take a callback
//===----------------------------------------------------------------------===//

Expr *Sema::checkCallbackMethod(CallExpr *call, MemberExpr *member) {
  enum Kind { ForEach, Map, FlatMap, Filter, Reduce, Find, FindIndex, Test, Sort };
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    Kind kind;
  };
  static const Method methods[] = {
      {"forEach", Builtin::ArrForEach, ForEach},
      {"map", Builtin::ArrMap, Map},
      {"flatMap", Builtin::ArrFlatMap, FlatMap},
      {"filter", Builtin::ArrFilter, Filter},
      {"reduce", Builtin::ArrReduce, Reduce},
      {"reduceRight", Builtin::ArrReduceRight, Reduce},
      {"find", Builtin::ArrFind, Find},
      {"findLast", Builtin::ArrFindLast, Find},
      {"findIndex", Builtin::ArrFindIndex, FindIndex},
      {"findLastIndex", Builtin::ArrFindLastIndex, FindIndex},
      {"some", Builtin::ArrSome, Test},
      {"every", Builtin::ArrEvery, Test},
  };
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  llvm::StringRef name = member->name;
  const Method *method = nullptr;
  for (const Method &m : methods)
    if (m.name == name)
      method = &m;
  Kind kind = method ? method->kind : Sort;
  size_t count = call->args.size();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  size_t maxArgs = kind == Reduce ? 2 : 1;
  if (count == 0) {
    diags_.error(member->loc(), "'" + name + "' needs a function");
    return call;
  }
  if (count > maxArgs) {
    if (count == 2 && kind != Reduce)
      diags_.error(call->args[1]->loc(), "the second argument of '" + name +
                                             "' sets 'this' for the callback, which is not supported");
    else
      diags_.error(member->loc(), "expected " + argumentCount(1, unsigned(maxArgs)) + ", but got " +
                                      llvm::Twine(count));
    for (size_t i = maxArgs; i < count; ++i)
      call->args[i] = checkExpr(call->args[i]);
    call->args.resize(maxArgs);
    count = maxArgs;
  }

  Signature sig;
  sig.pad = false;
  unsigned primary = 1;
  const Type *accumulator = element;
  switch (kind) {
  case ForEach:
    sig.voidResult = true;
    break;
  case Map:
  case FlatMap:
    break;
  case Filter:
  case Find:
  case FindIndex:
  case Test:
    sig.ret = Type::boolTy();
    break;
  case Reduce:
    primary = 2;
    if (count == 2) {
      // `reduce(f, 0)` over doubles starts from the double 0.
      Expr *&init = call->args[1];
      auto *unary = dyn_cast<UnaryExpr>(init);
      bool literal = isa<IntLitExpr>(init) || (unary && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand));
      init = checkExpr(init, literal && element->isNumeric() ? element : nullptr);
      accumulator = init->type;
      if (accumulator->isUndefined() || accumulator->isVoid()) {
        diags_.error(init->loc(), "the initial value of '" + name + "' needs a type; declare it in a "
                                                                   "variable first, e.g. 'const start: int | undefined = undefined'");
        accumulator = Type::error();
      }
    }
    sig.ret = accumulator;
    break;
  case Sort:
    primary = 2;
    break;
  }
  if (accumulator->isError()) {
    call->args[0] = checkExpr(call->args[0]);
    return call;
  }
  if (kind == Sort)
    sig.params = {element, element};
  else if (kind == Reduce)
    sig.params = {accumulator, element, Type::intTy(), arrayType};
  else
    sig.params = {element, Type::intTy(), arrayType};

  call->args[0] = checkFunctionArg(call->args[0], sig, primary, name);
  const Type *callback = call->args[0]->type;
  call->builtin = method ? method->builtin : Builtin::ArrSortWith;
  calledFunction(); // the callback may change anything
  if (!callback->isFunction())
    return call;
  const Type *result = callback->ret();
  switch (kind) {
  case ForEach:
    call->type = Type::voidTy();
    break;
  case Map:
  case FlatMap:
    if (result->isVoid() || result->isUndefined()) {
      diags_.error(call->args[0]->loc(), "the callback of '" + name + "' must return a value");
      return call;
    }
    call->type = kind == FlatMap && result->isArray() ? result : Type::array(result);
    break;
  case Filter:
    call->type = arrayType;
    break;
  case Reduce:
    call->type = accumulator;
    if (count == 2 && call->args.size() == 2)
      call->args[1] = coerce(call->args[1], accumulator);
    break;
  case Find:
    call->type = Type::optional(element);
    break;
  case FindIndex:
    call->type = Type::intTy();
    break;
  case Test:
    call->type = Type::boolTy();
    break;
  case Sort:
    if (!result->isNumeric()) {
      diags_.error(call->args[0]->loc(), "a comparison function must return a number (negative, zero "
                                         "or positive), found '" + result->name() + "'");
      return call;
    }
    call->type = arrayType;
    break;
  }
  return call;
}

} // namespace tz
