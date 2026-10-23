#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"

#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

// The names a statement mentions, to know which globals functions may use.
// With `inFunction` unset, only the names inside the functions it contains
// count.
void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction) {
  if (!stmt)
    return;
  auto expr = [&](const Expr *e) { collectNames(e, names, inFunction); };
  auto sub = [&](const Stmt *s) { collectNames(s, names, inFunction); };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body)
      sub(s);
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
      if (d.init)
        expr(d.init);
    break;
  case Stmt::Kind::Function: {
    auto *fn = cast<FunctionDecl>(stmt);
    for (const Param &param : fn->params)
      if (param.defaultValue)
        collectNames(param.defaultValue, names, true);
    collectNames(fn->body, names, true);
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
    expr(cast<DoWhileStmt>(stmt)->cond);
    sub(cast<DoWhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    sub(s->init);
    if (s->cond)
      expr(s->cond);
    if (s->step)
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
      if (c.test)
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
    if (const Expr *value = cast<ReturnStmt>(stmt)->value)
      expr(value);
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break;
  }
}

void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction) {
  auto sub = [&](const Expr *e) { collectNames(e, names, inFunction); };
  switch (expr->kind()) {
  case Expr::Kind::Name:
    if (inFunction)
      names.insert(cast<NameExpr>(expr)->name);
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
      sub(e);
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
      sub(e.expr);
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
    collectNames(cast<FunctionExpr>(expr)->fn, names, true);
    break;
  default:
    break;
  }
}

void Sema::check(SourceFile &file) {
  file_ = &file;
  pushScope(); // functions and globals
  declareTypes(file.items);

  // Functions are visible in the whole file, as in JavaScript.
  std::vector<FunctionDecl *> functions;
  for (Stmt *item : file.items)
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      declareFunction(fn);
      functions.push_back(fn);
    }
  findCycles(functions);

  // A global no function mentions cannot change during a call, and can live in
  // main's stack frame. That includes the arrows of top-level code.
  for (Stmt *item : file.items)
    collectNames(item, namesInFunctions_, /*inFunction=*/isa<FunctionDecl>(item));

  for (Stmt *item : file.items)
    if (auto *decl = dyn_cast<VarDeclStmt>(item))
      for (const VarDeclarator &d : decl->decls)
        topLevelNames_.insert(d.name);

  // Top-level code runs in order; its `let` and `const` become globals.
  flow_ = Flow();
  for (Stmt *item : file.items)
    if (!isa<FunctionDecl>(item))
      checkStmt(item);
  topLevelDone_ = true;

  // Bodies come last so that functions can use globals declared after them,
  // except those whose return type top-level code needed first. Nothing is
  // known about the globals when a function starts.
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (!fn->sym->ret)
        inferReturnType(fn->sym);
      if (fn->checked)
        continue;
      flow_ = Flow();
      checkFunctionBody(fn);
    }
  }

  popScope();
}

//===----------------------------------------------------------------------===//
// Scopes and types
//===----------------------------------------------------------------------===//

void Sema::findCycles(llvm::ArrayRef<FunctionDecl *> functions) {
  // Tarjan's algorithm over the calls between functions without a return
  // type. A name that only looks like such a call (a variable with the same
  // name) costs a few more checks, nothing else.
  std::vector<FunctionDecl *> nodes;
  llvm::StringMap<size_t> byName;
  for (FunctionDecl *fn : functions)
    if (fn->sym && !fn->sym->ret) {
      byName[fn->name] = nodes.size();
      nodes.push_back(fn);
    }
  std::vector<std::vector<size_t>> calls(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    llvm::StringSet<> names;
    for (const Param &param : nodes[i]->params)
      if (param.defaultValue)
        collectNames(param.defaultValue, names);
    collectNames(nodes[i]->body, names);
    for (const auto &name : names)
      if (auto found = byName.find(name.getKey()); found != byName.end())
        calls[i].push_back(found->second);
  }

  std::vector<int> index(nodes.size(), -1), low(nodes.size(), 0);
  std::vector<bool> onStack(nodes.size(), false);
  std::vector<size_t> stack;
  int next = 0;
  std::function<void(size_t)> visit = [&](size_t v) {
    index[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    for (size_t w : calls[v]) {
      if (index[w] < 0) {
        visit(w);
        low[v] = std::min(low[v], low[w]);
      } else if (onStack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] != index[v])
      return;
    std::vector<FuncSymbol *> cycle;
    size_t w;
    do {
      w = stack.back();
      stack.pop_back();
      onStack[w] = false;
      cycle.push_back(nodes[w]->sym);
    } while (w != v);
    if (cycle.size() == 1 && !llvm::is_contained(calls[v], v))
      return; // not recursive
    for (FuncSymbol *fn : cycle)
      cycleOf_[fn] = static_cast<unsigned>(cycles_.size());
    cycles_.push_back(std::move(cycle));
  };
  for (size_t v = 0; v < nodes.size(); ++v)
    if (index[v] < 0)
      visit(v);
}

void Sema::declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry) {
  if (name == "undefined") {
    diags_.error(loc, "'undefined' cannot be used as a name");
    return;
  }
  auto [it, inserted] = scopes_.back().try_emplace(name, entry);
  if (inserted)
    return;
  diags_.error(loc, "duplicate declaration of '" + name + "'");
  const ScopeEntry &previous = it->second;
  diags_.note(previous.var    ? previous.var->loc
              : previous.func ? previous.func->loc
                              : previous.enumDecl->loc(),
              "previously declared here");
}

Sema::ScopeEntry Sema::lookup(llvm::StringRef name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end())
      return found->second;
  }
  return {};
}

VarSymbol *Sema::declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst) {
  VarSymbol *var = ctx_.newVar();
  var->name = name;
  var->loc = loc;
  var->type = type;
  var->isConst = isConst;
  var->isGlobal = scopes_.size() == 1;
  var->usedInFunction = var->isGlobal && namesInFunctions_.contains(name);
  var->owner = var->isGlobal ? nullptr : currentDecl();
  var->declSeq = ++seq_;
  declare(name, loc, {var, nullptr});
  return var;
}

const Type *Sema::resolveType(const TypeRef &ref, bool allowVoid) {
  const Type *type = resolveTypeInner(ref);
  if (type->isVoid() && !allowVoid) {
    diags_.error(ref.loc, "'void' can only be used as a return type");
    return Type::error();
  }
  if (type->isUndefined()) {
    diags_.error(ref.loc, "the type 'undefined' alone is not supported; use 'T | undefined'");
    return Type::error();
  }
  return type;
}

const Type *Sema::resolveTypeInner(const TypeRef &ref) {
  switch (ref.kind) {
  case TypeRef::Kind::Name: {
    if (ref.name.empty())
      return Type::error(); // the parser already reported it
    if (const Type *type = Type::fromName(ref.name))
      return type;
    if (Stmt *decl = lookupType(ref.name)) {
      if (auto *alias = dyn_cast<TypeAliasDecl>(decl))
        return resolveAlias(alias);
      auto *e = cast<EnumDecl>(decl);
      resolveEnum(e);
      return e->isString ? Type::string() : Type::intTy();
    }
    if (ref.name == "Array")
      diags_.error(ref.loc, "'Array' needs an element type, e.g. 'Array<int>' or 'int[]'");
    else
      diags_.error(ref.loc, "cannot find type '" + ref.name + "'");
    return Type::error();
  }
  case TypeRef::Kind::Generic:
    // ReadonlyArray<T> is for tsc; changes to it are not stopped here.
    if (ref.name == "Array" || ref.name == "ReadonlyArray") {
      if (ref.args.size() != 1) {
        diags_.error(ref.loc, "'" + ref.name + "' takes one type argument");
        return Type::error();
      }
      TypeRef array;
      array.kind = TypeRef::Kind::Array;
      array.loc = ref.loc;
      array.args = ref.args;
      return resolveTypeInner(array);
    }
    diags_.error(ref.loc, "generic type '" + ref.name + "' is not supported");
    return Type::error();
  case TypeRef::Kind::Array: {
    const Type *element = resolveTypeInner(ref.args[0]);
    if (element->isVoid() || element->isUndefined()) {
      diags_.error(ref.loc, "arrays of '" + element->name() + "' are not supported");
      return Type::error();
    }
    return Type::array(element);
  }
  case TypeRef::Kind::Union: {
    bool withUndefined = false;
    const Type *base = nullptr;
    for (const TypeRef &member : ref.args) {
      const Type *type = resolveTypeInner(member);
      if (type->isError())
        return type;
      if (type->isUndefined() || type->isOptional())
        withUndefined = true;
      const Type *value = type->isUndefined() ? nullptr : type->nonOptional();
      if (!value || value == base)
        continue;
      if (base) {
        diags_.error(ref.loc, "union types other than 'T | undefined' are not supported yet");
        return Type::error();
      }
      base = value;
    }
    if (!base)
      return Type::undefinedTy();
    if (base->isVoid()) {
      diags_.error(ref.loc, "'void' cannot be part of a union");
      return Type::error();
    }
    return withUndefined ? Type::optional(base) : base;
  }
  case TypeRef::Kind::Function: {
    // `(a: int, b?: int, ...rest: int[]) => T`
    std::vector<const Type *> params;
    size_t minArgs = 0;
    bool optional = false, ok = true;
    size_t count = ref.args.size() - 1;
    for (size_t i = 0; i < count; ++i) {
      const Type *type = resolveType(ref.args[i], /*allowVoid=*/false);
      ok &= !type->isError();
      bool rest = ref.rest && i + 1 == count;
      if (rest) {
        if (!type->isArray() && !type->isError()) {
          diags_.error(ref.args[i].loc, "a rest parameter must be an array, e.g. '...xs: int[]'");
          ok = false;
        }
      } else if (ref.optional[i]) {
        type = Type::optional(type);
        optional = true;
      } else {
        if (optional) {
          diags_.error(ref.args[i].loc, "a required parameter cannot follow an optional one");
          ok = false;
        }
        minArgs = i + 1;
      }
      params.push_back(type);
    }
    const Type *ret = resolveType(ref.args.back(), /*allowVoid=*/true);
    if (!ok || ret->isError())
      return Type::error();
    return Type::function(params, minArgs, ref.rest, ret);
  }
  }
  llvm_unreachable("unknown type reference");
}

bool Sema::isImplicitlyConvertible(const Type *from, const Type *to) {
  if (from == to)
    return true;
  // Widening, as in C#. A long beyond 2^53 loses precision as a double, as
  // every such number does in JavaScript.
  if (from == Type::intTy() && (to == Type::longTy() || to == Type::doubleTy()))
    return true;
  if (from == Type::longTy() && to == Type::doubleTy())
    return true;
  if (to->isOptional())
    return from->isUndefined() || isImplicitlyConvertible(from->nonOptional(), to->inner());
  return false;
}

const Type *Sema::commonNumericType(const Type *a, const Type *b) {
  if (isImplicitlyConvertible(a, b))
    return b;
  if (isImplicitlyConvertible(b, a))
    return a;
  return nullptr;
}

const Type *Sema::unify(const Type *a, const Type *b) {
  if (a == b)
    return a;
  if (a->isError() || b->isError())
    return Type::error();
  if (a->isVoid() || b->isVoid())
    return nullptr;
  if (a->isUndefined())
    return Type::optional(b);
  if (b->isUndefined())
    return Type::optional(a);
  const Type *x = a->nonOptional(), *y = b->nonOptional();
  const Type *base = x == y ? x : x->isNumeric() && y->isNumeric() ? commonNumericType(x, y) : nullptr;
  if (!base)
    return nullptr;
  return a->isOptional() || b->isOptional() ? Type::optional(base) : base;
}

Expr *Sema::convert(Expr *expr, const Type *to) {
  if (expr->type == to || expr->type->isError() || to->isError())
    return expr;
  // An integer literal becomes a literal of the wider type, so that `-0` in
  // `[1.5, -0]` stays a negative zero.
  if (to->isNumeric() && expr->type->isInteger() && isImplicitlyConvertible(expr->type, to)) {
    auto *unary = dyn_cast<UnaryExpr>(expr);
    auto *lit = dyn_cast<IntLitExpr>(unary && unary->op == UnaryOp::Neg ? unary->operand : expr);
    if (lit) {
      lit->type = to;
      if (unary)
        unary->type = to;
      return expr;
    }
  }
  auto *cast = ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;
  return cast;
}

Expr *Sema::coerce(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  if (from == to || from->isError() || to->isError())
    return expr;
  if (isImplicitlyConvertible(from, to))
    return convert(expr, to);
  // A function named here can be given a type that takes more or fewer
  // arguments.
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  if (from->isFunction() && function->isFunction())
    if (Expr *adapted = adaptFunction(expr, function))
      return function == to ? adapted : convert(adapted, to);

  std::string message =
      ("type '" + from->name() + "' is not assignable to type '" + to->name() + "'").str();
  if (from->isFunction() && function->isFunction())
    message += "; the types of functions must match exactly: wrap it in an arrow that converts "
               "what it needs";
  if (from->isOptional() && isImplicitlyConvertible(from->inner(), to))
    message += "; the value may be undefined: check it first (e.g. 'x !== undefined') or give a "
               "default with '??'";
  else if (from->isFloat() && to->isInt())
    message += "; drop the fraction with 'x | 0', or round with Math.round, Math.floor, "
               "Math.ceil or Math.trunc";
  else if (from->isFloat() && to->isInteger())
    message += "; round it with Math.round, Math.floor, Math.ceil or Math.trunc";
  else if (from->isInteger() && to->isInteger())
    message += "; Math.trunc(x) converts it and checks that it fits";
  diags_.error(expr->loc(), message);
  return expr;
}

void Sema::reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" +
                         lhs->name() + "' and '" + rhs->name() + "'")
                            .str();
  if (lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined())
    message += "; a value may be undefined: check it first or give a default with '??'";
  diags_.error(loc, message);
}

const Type *Sema::comparisonType(const Type *a, const Type *b) {
  const Type *common = a->isVoid() || b->isVoid() ? nullptr : unify(a, b);
  if (common && common->isError())
    return common;
  const Type *base = common ? common->nonOptional() : nullptr;
  if (base && (base->isNumeric() || base->isBool() || base->isString() || base->isArray() ||
               base->isFunction()))
    return common;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Declarations and statements
//===----------------------------------------------------------------------===//

// The type of a parameter without an annotation, from a default value that
// is a literal, as `let x = 5` would give it; null for other values.
const Type *literalType(const Expr *value) {
  if (auto *unary = dyn_cast<UnaryExpr>(value); unary && unary->op == UnaryOp::Neg)
    value = unary->operand;
  if (auto *lit = dyn_cast<IntLitExpr>(value))
    return lit->value < (uint64_t(1) << 31)   ? Type::intTy()
           : lit->value < (uint64_t(1) << 63) ? Type::longTy()
                                               : Type::doubleTy();
  if (isa<FloatLitExpr>(value))
    return Type::doubleTy();
  if (isa<StringLitExpr>(value) || isa<TemplateExpr>(value))
    return Type::string();
  if (isa<BoolLitExpr>(value))
    return Type::boolTy();
  return nullptr;
}

void Sema::declareFunction(FunctionDecl *fn, VarSymbol *var) {
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  bool afterOptional = false;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *type = nullptr;
    if (param.type) {
      type = resolveType(*param.type, /*allowVoid=*/false);
    } else if (param.defaultValue && (type = literalType(param.defaultValue))) {
      // `function f(n = 10)`: n is an int.
    } else if (param.defaultValue && !param.rest && !fn->source.empty()) {
      // `function f(n = limit * 2)`: the type of the value, found when
      // needed (see resolveParams).
      param.declaredType = nullptr;
      sym->params.push_back(nullptr);
      sym->pendingParams = true;
      continue;
    } else {
      diags_.error(param.loc, "parameter '" + param.name + "' needs a type annotation");
      type = Type::error();
    }
    if (param.optional)
      type = Type::optional(type);
    param.declaredType = type;
    if (param.rest) {
      // The arguments from here on, as an array (empty without any).
      if (!type->isArray() && !type->isError()) {
        diags_.error(param.loc, "a rest parameter must be an array, e.g. '..." + param.name +
                                    ": int[]'");
        param.declaredType = Type::error();
      }
      sym->hasRest = true;
      sym->params.push_back(param.declaredType);
      continue;
    }
    // A caller may leave out an optional parameter or one with a default
    // value, or pass undefined: then the default applies.
    bool mayOmit = param.optional || param.defaultValue;
    sym->params.push_back(mayOmit ? Type::optional(type) : type);
    if (!mayOmit) {
      if (afterOptional)
        diags_.error(param.loc, "a required parameter cannot follow an optional one");
      sym->minArgs = i + 1;
    }
    afterOptional |= param.optional;
  }
  // Without an annotation: void if no `return` gives a value, else what the
  // body returns, found when needed (see returnType).
  if (fn->returnType)
    sym->ret = resolveType(*fn->returnType, /*allowVoid=*/true);
  else if (!returnsValue(fn->body))
    sym->ret = Type::voidTy();
  fn->sym = sym;
  if (!fn->name.empty())
    declare(fn->name, fn->loc(), {var, sym});
}

void Sema::checkFunctionBody(FunctionDecl *fn) {
  FuncSymbol *sym = fn->sym;
  resolveParams(sym);
  fn->checked = true;
  FuncSymbol *outer = currentFunction_;
  currentFunction_ = sym;
  functions_.push_back(fn);
  pushScope();
  for (size_t i = 0; i < fn->params.size(); ++i) {
    // A default value is computed at each call that leaves the parameter
    // out, where it sees the parameters before it.
    Param &param = fn->params[i];
    const Type *type = param.declaredType;
    if (param.defaultValue && type->isError() && !param.type) {
      // Its type comes from it, and checking it failed (see resolveParams).
      param.defaultValue->type = type;
      param.sym = declareVar(param.name, param.loc, type, /*isConst=*/false);
      continue;
    }
    if (param.defaultValue) {
      llvm::StringSet<> names;
      collectNames(param.defaultValue, names);
      bool forward = false;
      for (size_t j = i; j < fn->params.size() && !forward; ++j)
        if (names.contains(fn->params[j].name)) {
          diags_.error(param.defaultValue->loc(), "the default value of '" + param.name + "' uses '" +
                                                      fn->params[j].name +
                                                      "', which is not set yet: a default value can "
                                                      "use only the parameters before it");
          forward = true;
        }
      if (forward) {
        param.defaultValue->type = Type::error();
        param.sym = declareVar(param.name, param.loc, type, /*isConst=*/false);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(param.defaultValue); fe && fe->fn->name.empty())
        fe->fn->name = param.name; // as JavaScript names it
      param.defaultValue = checkExpr(param.defaultValue, type->isError() ? nullptr : type);
      param.defaultValue = coerce(param.defaultValue, type);
    }
    param.sym = declareVar(param.name, param.loc, type, /*isConst=*/false);
    if (param.defaultValue && type->isOptional() && !param.defaultValue->type->isOptional() &&
        !param.defaultValue->type->isUndefined())
      setDefined(param.sym, true);
  }
  // The parameters and the outermost block of the body share one scope.
  checkStatements(fn->body);
  popScope();
  functions_.pop_back();
  currentFunction_ = outer;

  // As in TypeScript, a function that may return undefined may also just end.
  const Type *ret = sym->ret;
  if (ret && !ret->isVoid() && !ret->isError() && !ret->isOptional() && !alwaysReturns(fn->body))
    diags_.error(fn->loc(), functionLabel(fn->name) + " must return a value of type '" +
                                ret->name() + "' on every path");
}

void Sema::checkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    pushScope();
    checkStatements(cast<BlockStmt>(stmt));
    popScope();
    break;
  case Stmt::Kind::VarDecl:
    checkVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break; // declared with the block (declareTypes)
  case Stmt::Kind::Function:
    // check() handles top-level functions, and checkStatements those of a
    // block.
    if (!cast<FunctionDecl>(stmt)->group)
      diags_.error(stmt->loc(), "a function can be declared only directly in a block; in a "
                                "'switch', declare it before the 'switch'");
    break;
  case Stmt::Kind::Expr: {
    auto *s = cast<ExprStmt>(stmt);
    s->expr = checkExpr(s->expr);
    break;
  }
  case Stmt::Kind::If:
    checkIf(cast<IfStmt>(stmt));
    break;
  case Stmt::Kind::While:
    checkWhile(cast<WhileStmt>(stmt));
    break;
  case Stmt::Kind::DoWhile:
    checkDoWhile(cast<DoWhileStmt>(stmt));
    break;
  case Stmt::Kind::For:
    checkFor(cast<ForStmt>(stmt));
    break;
  case Stmt::Kind::ForOf:
    checkForOf(cast<ForOfStmt>(stmt));
    break;
  case Stmt::Kind::Switch:
    checkSwitch(cast<SwitchStmt>(stmt));
    break;
  case Stmt::Kind::Labeled:
    checkLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    checkReturn(cast<ReturnStmt>(stmt));
    flow_.unreachable = true;
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    checkJump(stmt);
    flow_.unreachable = true;
    break;
  case Stmt::Kind::Empty:
    break;
  }
}

void Sema::pushTarget(JumpTarget::Kind kind) {
  JumpTarget target{kind, std::move(pendingLabels_), Flow()};
  pendingLabels_.clear();
  target.breaks.unreachable = true;
  targets_.push_back(std::move(target));
}

void Sema::checkJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  const char *keyword = isBreak ? "break" : "continue";
  JumpTarget *target = nullptr;
  for (JumpTarget &t : llvm::reverse(targets_)) {
    bool matches = label.empty() ? (isBreak ? t.kind != JumpTarget::Kind::Block
                                            : t.kind == JumpTarget::Kind::Loop)
                                 : llvm::is_contained(t.labels, label);
    if (matches) {
      target = &t;
      break;
    }
  }
  if (!target) {
    if (!label.empty())
      diags_.error(stmt->loc(), "cannot find label '" + label + "'");
    else if (isBreak)
      diags_.error(stmt->loc(), "'break' can only be used inside a loop or a 'switch'");
    else
      diags_.error(stmt->loc(), "'continue' can only be used inside a loop");
    return;
  }
  if (!isBreak && target->kind != JumpTarget::Kind::Loop) {
    diags_.error(stmt->loc(), llvm::Twine("'") + keyword + " " + label +
                                  "' needs a loop, and '" + label + "' does not label one");
    return;
  }
  if (isBreak && target->kind != JumpTarget::Kind::Loop)
    target->breaks = join(target->breaks, flow_);
}

static bool isLoop(const Stmt *stmt) {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) || isa<ForOfStmt>(stmt);
}

void Sema::checkLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    bool duplicate = llvm::is_contained(labels, labeled->label) ||
                     llvm::any_of(targets_, [&](const JumpTarget &t) {
                       return llvm::is_contained(t.labels, labeled->label);
                     });
    if (duplicate)
      diags_.error(labeled->loc(), "duplicate label '" + labeled->label + "'");
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isLoop(body) || isa<SwitchStmt>(body)) {
    pendingLabels_ = labels; // for the loop or switch to take
    checkStmt(body);
    return;
  }
  if (isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) || isa<TypeAliasDecl>(body) ||
      isa<EnumDecl>(body)) {
    diags_.error(body->loc(), "a declaration cannot have a label");
    return;
  }
  // `label: { ... break label; ... }`
  pendingLabels_ = labels;
  pushTarget(JumpTarget::Kind::Block);
  checkStmt(body);
  Flow breaks = targets_.back().breaks;
  targets_.pop_back();
  flow_ = join(flow_, breaks);
}

// A literal case test: comparing with it cannot change any variable, so what
// the comparison says about the discriminant holds in the case's body.
static bool isConstantTest(const Expr *test) {
  if (auto *unary = dyn_cast<UnaryExpr>(test); unary && unary->op == UnaryOp::Neg)
    test = unary->operand;
  return isa<IntLitExpr>(test) || isa<FloatLitExpr>(test) || isa<StringLitExpr>(test) ||
         isa<BoolLitExpr>(test) || isa<UndefinedExpr>(test);
}

// The tests run in order until one matches; then control enters that case
// (or `default` if none matches) and falls through the cases after it until
// a `break`.
void Sema::checkSwitch(SwitchStmt *stmt) {
  stmt->discriminant = checkExpr(stmt->discriminant);
  const Type *type = stmt->discriminant->type;
  if (type->isVoid()) {
    diags_.error(stmt->discriminant->loc(), "an expression of type 'void' cannot be compared");
    type = Type::error();
  }
  // As in JavaScript, the cases share one scope.
  pushScope();
  std::vector<Stmt *> all;
  for (SwitchCase &c : stmt->cases)
    all.insert(all.end(), c.body.begin(), c.body.end());
  declareTypes(all);

  const Type *hint = type->nonOptional()->isNumeric() ? type->nonOptional() : nullptr;
  Flow tests = flow_; // what holds while the tests run
  std::vector<Flow> matched(stmt->cases.size());
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    if (!c.test)
      continue;
    flow_ = tests;
    c.test = checkExpr(c.test, hint);
    const Type *testType = c.test->type;
    Flow whenTrue = flow_, whenFalse = flow_;
    if (!type->isError() && !testType->isError() && !testType->isUndefined()) {
      const Type *common = comparisonType(type, testType);
      if (!common) {
        diags_.error(c.test->loc(), "a case of type '" + testType->name() +
                                        "' cannot be compared with a 'switch' value of type '" +
                                        type->name() + "'");
      } else if (!common->isError()) {
        if (common->nonOptional()->isFunction())
          file_->comparesFunctions = true;
        // A literal discriminant has nothing to learn about, and converting
        // it here would change its type.
        bool constant = isConstantTest(c.test) && !isConstantTest(stmt->discriminant);
        c.test = convert(c.test, common);
        c.compareType = common;
        if (constant) {
          // `discriminant === test`, to learn from it as from a condition.
          BinaryExpr equal(c.loc, BinaryOp::Eq, convert(stmt->discriminant, common), c.test);
          equal.type = Type::boolTy();
          addConditionFacts(&equal, whenTrue, whenFalse);
        }
      }
    } else if (testType->isUndefined() && isa<UndefinedExpr>(c.test)) {
      BinaryExpr equal(c.loc, BinaryOp::Eq, stmt->discriminant, c.test);
      equal.type = Type::boolTy();
      addConditionFacts(&equal, whenTrue, whenFalse);
    }
    matched[i] = whenTrue;
    tests = whenFalse;
  }

  pushTarget(JumpTarget::Kind::Switch);
  Flow fallthrough;
  fallthrough.unreachable = true;
  bool hasDefault = false;
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    hasDefault |= !c.test;
    flow_ = join(fallthrough, c.test ? matched[i] : tests);
    for (Stmt *s : c.body)
      checkStmt(s);
    fallthrough = flow_;
    // A variable declared in this case is in scope in the cases after it,
    // but JavaScript reports its use there as an error unless control fell
    // through its declaration. Sema rejects such uses outright.
    for (Stmt *s : c.body)
      if (auto *decl = dyn_cast<VarDeclStmt>(s))
        for (const VarDeclarator &d : decl->decls)
          if (d.sym)
            otherCaseVars_.insert(d.sym);
  }
  Flow exit = fallthrough;
  if (!hasDefault)
    exit = join(exit, tests);
  exit = join(exit, targets_.back().breaks);
  targets_.pop_back();
  for (SwitchCase &c : stmt->cases)
    for (Stmt *s : c.body)
      if (auto *decl = dyn_cast<VarDeclStmt>(s))
        for (const VarDeclarator &d : decl->decls)
          otherCaseVars_.erase(d.sym);
  popScope();
  flow_ = exit;
}

void Sema::checkIf(IfStmt *stmt) {
  Flow whenTrue, whenFalse;
  stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  flow_ = whenTrue;
  checkBody(stmt->thenStmt);
  Flow afterThen = flow_;
  flow_ = whenFalse;
  if (stmt->elseStmt)
    checkBody(stmt->elseStmt);
  flow_ = join(afterThen, flow_);
}

// Loops are checked once. Their head state comes from scanning the loop for
// what it changes (loopHead), so what Sema assumes at the start of the body
// holds on every iteration, and everywhere in the loop.
void Sema::checkWhile(WhileStmt *stmt) {
  LoopScan scan = scanLoop({stmt->body}, {stmt->cond});
  Flow head = loopHead(scan, stmt->cond, /*induction=*/true);
  flow_ = head;
  Flow whenTrue, whenFalse;
  stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  flow_ = whenTrue;
  checkLoopBody(stmt->body);
  flow_ = scan.breaks ? join(whenFalse, head) : whenFalse;
}

void Sema::checkDoWhile(DoWhileStmt *stmt) {
  // No induction: the body runs once before the condition is first checked.
  LoopScan scan = scanLoop({stmt->body}, {stmt->cond});
  Flow head = loopHead(scan, nullptr, /*induction=*/false);
  flow_ = head;
  checkLoopBody(stmt->body);
  flow_ = join(flow_, head); // `continue` jumps to the condition
  Flow whenTrue, whenFalse;
  stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  flow_ = scan.breaks ? join(whenFalse, head) : whenFalse;
}

void Sema::checkFor(ForStmt *stmt) {
  pushScope(); // variables declared in the loop header
  if (stmt->init)
    checkStmt(stmt->init);
  LoopScan scan = scanLoop({stmt->body}, {stmt->cond, stmt->step});
  Flow head = loopHead(scan, stmt->cond, /*induction=*/true);
  flow_ = head;
  Flow whenTrue = head, whenFalse = head;
  whenFalse.unreachable = true;
  if (stmt->cond)
    stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  flow_ = whenTrue;
  checkLoopBody(stmt->body);
  if (stmt->step) {
    flow_ = head;
    stmt->step = checkExpr(stmt->step);
  }
  flow_ = scan.breaks ? join(whenFalse, head) : whenFalse;
  popScope();
}

void Sema::checkForOf(ForOfStmt *stmt) {
  stmt->iterable = checkExpr(stmt->iterable);
  const Type *type = stmt->iterable->type;
  const Type *element = Type::error();
  if (type->isArray())
    element = type->inner();
  else if (type->isString())
    element = Type::string();
  else if (type->isOptional() && (type->inner()->isArray() || type->inner()->isString()))
    diags_.error(stmt->iterable->loc(), "'" + type->name() +
                                            "' may be undefined; check it before iterating");
  else if (!type->isError())
    diags_.error(stmt->iterable->loc(), "cannot iterate over a value of type '" + type->name() + "'");
  if (stmt->binding.typeAnn)
    diags_.error(stmt->binding.typeAnn->loc, "a 'for...of' variable cannot have a type annotation");
  if (stmt->binding.destructured && !element->isArray() && !element->isError()) {
    diags_.error(stmt->binding.loc, "only arrays can be destructured; the elements are '" +
                                        element->name() + "'");
    element = Type::error();
  }

  LoopScan scan = scanLoop({stmt->body}, {});
  // The array keeps each element alive during its iteration unless the body
  // could remove or replace elements, directly or through a function. Then a
  // const loop variable can borrow the element instead of counting it.
  stmt->borrowElements = stmt->isConst && type->isArray() && element->isManaged() &&
                         !scan.callsFunctions && !scan.releasesElements;
  Flow head = loopHead(scan, nullptr, /*induction=*/false);
  flow_ = head;
  pushScope();
  stmt->binding.sym =
      declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  checkLoopBody(stmt->body);
  popScope();
  flow_ = head;
}

void Sema::checkLoopBody(Stmt *body) {
  pushTarget(JumpTarget::Kind::Loop);
  checkBody(body);
  targets_.pop_back();
}

// The body of `if`, `while` or `for`. As in TypeScript, a declaration there
// needs braces: it would be visible nowhere else.
void Sema::checkBody(Stmt *body) {
  if (isa<VarDeclStmt>(body) || isa<TypeAliasDecl>(body) || isa<EnumDecl>(body) ||
      isa<FunctionDecl>(body)) {
    diags_.error(body->loc(), "a declaration cannot be the whole body here; wrap it in '{ }'");
    return;
  }
  checkStmt(body);
}

void Sema::checkVarDecl(VarDeclStmt *decl) {
  for (VarDeclarator &d : decl->decls) {
    const Type *declared = d.typeAnn ? resolveType(*d.typeAnn, /*allowVoid=*/false) : nullptr;
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init)) {
      if (fe->fn->name.empty())
        fe->fn->name = d.name; // as JavaScript names it
      if (decl->isConst && !d.destructured) {
        // The constant exists while its function is checked, so that the
        // function can call itself through it.
        VarSymbol *var = declareVar(d.name, d.loc, nullptr, /*isConst=*/true);
        var->lambda = fe->fn;
        d.sym = var;
        const Type *function = declared && declared->nonOptional()->isFunction() ? declared->nonOptional() : nullptr;
        Signature sig = function ? signatureOf(function) : Signature();
        d.init = checkFunctionExpr(fe, function ? &sig : nullptr);
        if (declared)
          d.init = coerce(d.init, declared);
        var->type = declared ? declared : d.init->type;
        continue;
      }
    }
    const Type *type = declared;
    Expr *value = nullptr; // the initializer before its conversion
    if (d.init) {
      d.init = value = checkExpr(d.init, declared);
      if (declared) {
        d.init = coerce(d.init, declared);
      } else {
        type = d.init->type;
        if (type->isVoid()) {
          diags_.error(d.init->loc(), "an expression of type 'void' cannot initialize a variable");
          type = Type::error();
        } else if (type->isUndefined()) {
          diags_.error(d.init->loc(), "cannot infer a type from 'undefined'; add one, e.g. 'let " +
                                          d.name + ": int | undefined = undefined'");
          type = Type::error();
        }
      }
    } else if (decl->isConst) {
      diags_.error(d.loc, "'const' declarations must be initialized");
      if (!type)
        type = Type::error();
    } else if (!declared) {
      diags_.error(d.loc, "variable '" + d.name + "' needs a type annotation or an initializer");
      type = Type::error();
    }

    if (d.destructured && type->isString() && !declared) {
      // A string destructures into its characters, as JavaScript's iterator
      // gives them: `[...s]`.
      auto *chars = ctx_.make<ArrayLitExpr>(d.init->loc());
      chars->elements.push_back({d.init, /*spread=*/true});
      chars->type = Type::array(Type::string());
      d.init = value = chars;
      type = chars->type;
    } else if (d.destructured && !type->isArray() && !type->isError()) {
      std::string message = "only arrays can be destructured, found '" + type->name().str() + "'";
      if (type->isOptional() && type->inner()->isArray())
        message += "; the array may be undefined: check it first";
      diags_.error(d.init ? d.init->loc() : d.loc, message);
      type = Type::error();
    }

    d.sym = declareVar(d.name, d.loc, type, decl->isConst);
    if (value)
      assigned(d.sym, value);
    if (decl->isConst && d.init && type->isInteger())
      if (std::optional<int64_t> constant = intConstant(d.init))
        constVars_[d.sym] = *constant;
  }
}

void Sema::checkReturn(ReturnStmt *ret) {
  if (!currentFunction_) {
    diags_.error(ret->loc(), "'return' can only be used inside a function");
    if (ret->value)
      ret->value = checkExpr(ret->value);
    return;
  }

  const FuncSymbol *fn = currentFunction_;
  if (!fn->ret) {
    // Its type comes from what its return statements give.
    if (ret->value)
      ret->value = checkExpr(ret->value);
    returns_->push_back(ret);
    return;
  }
  if (!ret->value) {
    if (!fn->ret->isVoid() && !fn->ret->isError() && !fn->ret->isOptional())
      diags_.error(ret->loc(), functionLabel(fn->name) + " must return a value of type '" +
                                   fn->ret->name() + "'");
    return;
  }

  ret->value = checkExpr(ret->value, fn->ret->isVoid() ? nullptr : fn->ret);
  if (fn->ret->isVoid()) {
    // `return f()` with a void `f` is fine, as in TypeScript, and so is any
    // value in a callback whose result is dropped.
    const Type *type = ret->value->type;
    if (!type->isVoid() && !type->isError() && !fn->discardsResult)
      diags_.error(ret->value->loc(), functionLabel(fn->name) + " is declared to return 'void', "
                                                               "but returns '" + type->name() + "'");
    return;
  }
  ret->value = coerce(ret->value, fn->ret);
}

Expr *Sema::checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse) {
  cond = checkBranching(cond, whenTrue, whenFalse);
  if (!cond->type->isBool() && !cond->type->isError())
    diags_.error(cond->loc(), "condition must be of type 'boolean', found '" + cond->type->name() + "'");
  return cond;
}

// Checks an expression whose truth matters, following `&&`, `||` and `!` so
// that the right side of `i < a.length && a[i] > 0` knows `i < a.length`.
Expr *Sema::checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse) {
  if (auto *bin = dyn_cast<BinaryExpr>(expr);
      bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)) {
    bool isAnd = bin->op == BinaryOp::LogAnd;
    Flow lhsTrue, lhsFalse, rhsTrue, rhsFalse;
    bin->lhs = checkBranching(bin->lhs, lhsTrue, lhsFalse);
    flow_ = isAnd ? lhsTrue : lhsFalse; // the right side runs only then
    bin->rhs = checkBranching(bin->rhs, rhsTrue, rhsFalse);
    whenTrue = isAnd ? rhsTrue : join(lhsTrue, rhsTrue);
    whenFalse = isAnd ? join(lhsFalse, rhsFalse) : rhsFalse;
    flow_ = join(whenTrue, whenFalse);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (!l->isError() && !r->isError() && !(l->isBool() && r->isBool()))
      reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    bin->type = Type::boolTy();
    return bin;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Not) {
    Flow operandTrue, operandFalse;
    unary->operand = checkBranching(unary->operand, operandTrue, operandFalse);
    whenTrue = operandFalse;
    whenFalse = operandTrue;
    const Type *type = unary->operand->type;
    if (!type->isBool() && !type->isError())
      diags_.error(unary->loc(), "operator '!' cannot be applied to type '" + type->name() + "'");
    unary->type = Type::boolTy();
    return unary;
  }

  expr = checkExpr(expr, Type::boolTy());
  whenTrue = flow_;
  whenFalse = flow_;
  if (auto *lit = dyn_cast<BoolLitExpr>(expr))
    (lit->value ? whenFalse : whenTrue).unreachable = true;
  else
    addConditionFacts(expr, whenTrue, whenFalse);
  return expr;
}

static bool isTrueLiteral(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<BoolLitExpr>(expr);
  return lit && lit->value;
}

namespace {

// Finds the `break` and `continue` statements inside a statement S that
// leave S: those that jump to S itself (except `continue` to a loop S) or to
// a statement around it.
class Exits {
public:
  Exits(bool isLoop, llvm::ArrayRef<llvm::StringRef> labels) : isLoop_(isLoop), labels_(labels) {}

  bool any(llvm::ArrayRef<Stmt *> stmts) {
    return llvm::any_of(stmts, [&](const Stmt *s) { return in(s); });
  }

  bool in(const Stmt *stmt) {
    switch (stmt->kind()) {
    case Stmt::Kind::Break: {
      llvm::StringRef label = cast<BreakStmt>(stmt)->label;
      return label.empty() ? breakables_ == 0 : !llvm::is_contained(inner_, label);
    }
    case Stmt::Kind::Continue: {
      llvm::StringRef label = cast<ContinueStmt>(stmt)->label;
      if (label.empty())
        return loops_ == 0 && !isLoop_;
      if (llvm::is_contained(inner_, label))
        return false;
      return !(isLoop_ && llvm::is_contained(labels_, label));
    }
    case Stmt::Kind::Block:
      return any(cast<BlockStmt>(stmt)->body);
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      return in(s->thenStmt) || (s->elseStmt && in(s->elseStmt));
    }
    case Stmt::Kind::While:
      return inLoop(cast<WhileStmt>(stmt)->body);
    case Stmt::Kind::DoWhile:
      return inLoop(cast<DoWhileStmt>(stmt)->body);
    case Stmt::Kind::For:
      return inLoop(cast<ForStmt>(stmt)->body);
    case Stmt::Kind::ForOf:
      return inLoop(cast<ForOfStmt>(stmt)->body);
    case Stmt::Kind::Switch: {
      ++breakables_;
      bool found = llvm::any_of(cast<SwitchStmt>(stmt)->cases,
                                [&](const SwitchCase &c) { return any(c.body); });
      --breakables_;
      return found;
    }
    case Stmt::Kind::Labeled: {
      auto *s = cast<LabeledStmt>(stmt);
      inner_.push_back(s->label);
      bool found = in(s->body);
      inner_.pop_back();
      return found;
    }
    default:
      return false;
    }
  }

private:
  bool inLoop(const Stmt *body) {
    ++loops_;
    ++breakables_;
    bool found = in(body);
    --loops_;
    --breakables_;
    return found;
  }

  bool isLoop_;
  llvm::ArrayRef<llvm::StringRef> labels_; // of S
  llvm::SmallVector<llvm::StringRef, 4> inner_; // declared inside S
  unsigned loops_ = 0, breakables_ = 0; // inside S, around the current statement
};

// Whether some `break` inside `stmt` jumps to one of `labels`.
bool breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  if (labels.empty())
    return false;
  switch (stmt->kind()) {
  case Stmt::Kind::Break:
    return llvm::is_contained(labels, cast<BreakStmt>(stmt)->label);
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body,
                        [&](const Stmt *s) { return breaksTo(s, labels); });
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    return breaksTo(s->thenStmt, labels) || (s->elseStmt && breaksTo(s->elseStmt, labels));
  }
  case Stmt::Kind::While:
    return breaksTo(cast<WhileStmt>(stmt)->body, labels);
  case Stmt::Kind::DoWhile:
    return breaksTo(cast<DoWhileStmt>(stmt)->body, labels);
  case Stmt::Kind::For:
    return breaksTo(cast<ForStmt>(stmt)->body, labels);
  case Stmt::Kind::ForOf:
    return breaksTo(cast<ForOfStmt>(stmt)->body, labels);
  case Stmt::Kind::Switch:
    return llvm::any_of(cast<SwitchStmt>(stmt)->cases, [&](const SwitchCase &c) {
      return llvm::any_of(c.body, [&](const Stmt *s) { return breaksTo(s, labels); });
    });
  case Stmt::Kind::Labeled:
    return breaksTo(cast<LabeledStmt>(stmt)->body, labels);
  default:
    return false;
  }
}

} // namespace

// Conservative: true only if control can never reach the end of `stmt`.
bool Sema::alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  auto returns = [](const Stmt *s) { return alwaysReturns(s); };
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
    return true;
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, returns) && !breaksTo(stmt, labels);
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    return s->elseStmt && alwaysReturns(s->thenStmt) && alwaysReturns(s->elseStmt) &&
           !breaksTo(stmt, labels);
  }
  // An endless loop never falls through unless something leaves it.
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    return (!s->cond || isTrueLiteral(s->cond)) && !Exits(true, labels).in(s->body);
  }
  // A switch with `default` always enters some case, and control falls
  // through to the end of the last one unless something leaves the switch.
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    bool hasDefault = llvm::any_of(s->cases, [](const SwitchCase &c) { return !c.test; });
    if (!hasDefault || s->cases.empty())
      return false;
    Exits exits(false, labels);
    if (llvm::any_of(s->cases, [&](const SwitchCase &c) { return exits.any(c.body); }))
      return false;
    return llvm::any_of(s->cases.back().body, returns);
  }
  case Stmt::Kind::Labeled: {
    llvm::SmallVector<llvm::StringRef, 2> all(labels.begin(), labels.end());
    while (auto *labeled = dyn_cast<LabeledStmt>(stmt)) {
      all.push_back(labeled->label);
      stmt = labeled->body;
    }
    return alwaysReturns(stmt, all);
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

Expr *Sema::checkExpr(Expr *expr, const Type *expected, bool wraps) {
  // A built-in such as Math.sqrt where a function is expected.
  if (expected && (isa<NameExpr>(expr) || isa<MemberExpr>(expr)))
    if (const Type *function = expected->nonOptional(); function->isFunction()) {
      Signature sig = signatureOf(function);
      if (FunctionExpr *wrapper = builtinFunctionValue(expr, sig, unsigned(sig.params.size()), {}))
        return checkFunctionExpr(wrapper, &sig);
    }
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    return checkIntLit(cast<IntLitExpr>(expr), expected, /*negated=*/false);
  case Expr::Kind::FloatLit:
    expr->type = Type::doubleTy();
    return expr;
  case Expr::Kind::BoolLit:
    expr->type = Type::boolTy();
    return expr;
  case Expr::Kind::StringLit:
    expr->type = Type::string();
    return expr;
  case Expr::Kind::Undefined:
    expr->type = Type::undefinedTy();
    return expr;
  case Expr::Kind::Template:
    return checkTemplate(cast<TemplateExpr>(expr));
  case Expr::Kind::ArrayLit:
    return checkArrayLit(cast<ArrayLitExpr>(expr), expected);
  case Expr::Kind::Name:
    return checkName(cast<NameExpr>(expr));
  case Expr::Kind::Member:
    return checkMember(cast<MemberExpr>(expr), expected);
  case Expr::Kind::Index:
    return checkIndex(cast<IndexExpr>(expr));
  case Expr::Kind::Unary:
    return checkUnary(cast<UnaryExpr>(expr), expected, wraps);
  case Expr::Kind::Binary:
    return checkBinary(cast<BinaryExpr>(expr), expected, wraps);
  case Expr::Kind::Assign:
    return checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return checkCall(cast<CallExpr>(expr), expected);
  case Expr::Kind::New:
    return checkNew(cast<NewExpr>(expr), expected);
  case Expr::Kind::Conditional:
    return checkConditional(cast<ConditionalExpr>(expr), expected);
  case Expr::Kind::Cast:
    return checkCast(cast<CastExpr>(expr));
  case Expr::Kind::Sequence:
    return checkSequence(cast<SequenceExpr>(expr), expected, wraps);
  case Expr::Kind::OptionalChain:
    return checkOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Function: {
    // Where a function type is expected, the parameters take its types.
    const Type *function = expected ? expected->nonOptional() : nullptr;
    if (function && function->isFunction()) {
      Signature sig = signatureOf(function);
      return checkFunctionExpr(cast<FunctionExpr>(expr), &sig);
    }
    return checkFunctionExpr(cast<FunctionExpr>(expr), nullptr);
  }
  }
  llvm_unreachable("unknown expression kind");
}

Expr *Sema::checkSequence(SequenceExpr *seq, const Type *expected, bool wraps) {
  for (size_t i = 0; i + 1 < seq->exprs.size(); ++i)
    seq->exprs[i] = checkExpr(seq->exprs[i]);
  Expr *&last = seq->exprs.back();
  last = checkExpr(last, expected, wraps);
  seq->type = last->type;
  return seq;
}

Expr *Sema::checkOptionalChain(OptionalChainExpr *chain) {
  bool outer = std::exchange(chainShortCircuits_, false);
  Flow before = flow_;
  chain->chain = checkExpr(chain->chain);
  chain->mayShortCircuit = chainShortCircuits_;
  chainShortCircuits_ = outer;
  const Type *type = chain->chain->type;
  chain->type = type;
  if (!chain->mayShortCircuit || type->isError())
    return chain;
  // The rest of the chain may not have run.
  flow_ = join(before, flow_);
  if (!type->isVoid())
    chain->type = Type::optional(type);
  return chain;
}

Expr *Sema::chainLink(Expr *object) {
  const Type *type = object->type;
  if (!type->isOptional())
    return object; // `?.` on a value that is always there is a plain `.`
  auto *link = ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type->inner();
  link->shortCircuit = true;
  chainShortCircuits_ = true;
  return link;
}

// An integer literal takes the numeric type the context expects; otherwise
// int, or long if it does not fit.
Expr *Sema::checkIntLit(IntLitExpr *lit, const Type *expected, bool negated) {
  auto fits = [&](unsigned bits) {
    uint64_t limit = uint64_t(1) << (bits - 1);
    return negated ? lit->value <= limit : lit->value < limit;
  };

  if (expected)
    expected = expected->nonOptional();
  if (expected && expected->isNumeric()) {
    lit->type = expected;
    if (expected->isInteger() && !fits(expected->bitWidth()))
      diags_.error(lit->loc(), "integer literal does not fit in type '" + expected->name() + "'");
    return lit;
  }
  if (fits(32))
    lit->type = Type::intTy();
  else if (fits(64))
    lit->type = Type::longTy();
  else // only a double holds it, as any number in JavaScript
    lit->type = Type::doubleTy();
  return lit;
}

const Type *Sema::varType(VarSymbol *var) {
  if (var->type)
    return var->type;
  // A function's variable, while its type is being worked out.
  FunctionDecl *fn = var->function ? var->function : var->lambda;
  if (!fn || !fn->sym)
    return Type::error();
  const Type *type = functionType(fn->sym);
  if (var->function && fn->sym->ret)
    var->type = type;
  return type;
}

Expr *Sema::checkName(NameExpr *name) {
  ScopeEntry entry = lookup(name->name);
  if (entry.var) {
    name->var = entry.var;
    noteUse(entry.var, name->loc());
    name->type = varType(entry.var);
    checkVisible(name, entry.var);
    // Narrowed: known to hold a value here.
    if (name->type->isOptional() && isDefined(entry.var))
      return convert(name, name->type->inner());
    return name;
  }
  if (!entry.func && !entry.enumDecl)
    if (Expr *constant = globalConstant("", name->name, name->loc()))
      return constant;
  if (entry.func) {
    // A top-level function as a value.
    name->func = entry.func;
    name->type = functionType(entry.func);
    return name;
  }
  static const llvm::StringLiteral builtins[] = {"String", "Number", "parseInt", "parseFloat", "isNaN",
                                                 "isFinite"};
  if (name->name == "this")
    diags_.error(name->loc(), "'this' exists only in the methods of classes, which are not supported yet");
  else if (llvm::is_contained(builtins, name->name))
    diags_.error(name->loc(), "'" + name->name + "' can be used as a value only where a function type "
                                                 "is expected, e.g. 'const f: (s: string) => double = " +
                                  name->name + "'");
  else if (name->name == "arguments")
    diags_.error(name->loc(), "'arguments' is not supported; use a rest parameter, e.g. '...args: int[]'");
  else if (entry.enumDecl)
    diags_.error(name->loc(), "enum '" + name->name + "' can only be used as '" + name->name +
                                  ".Member' or '" + name->name + "[value]'");
  else if (early_ && (topLevelNames_.contains(name->name) || declaredLater(name->name)))
    diags_.error(name->loc(), "'" + name->name + "' is declared after the code that first calls '" +
                                  early_->name + "', which needs to know what '" + early_->name +
                                  "' returns; move the declaration up, or declare the return type");
  else if (const BlockNames *later = declaredLater(name->name); later && functions_.size() > later->functions)
    diags_.error(name->loc(), "'" + name->name + "' is declared later in the block, after this function "
                                                 "is created; move the declaration up, or declare the "
                                                 "function with 'function', which can use it");
  else if (later)
    diags_.error(name->loc(), "'" + name->name + "' is used before it is declared");
  else
    diags_.error(name->loc(), "cannot find name '" + name->name + "'");
  name->type = Type::error();
  return name;
}

void Sema::checkVisible(NameExpr *name, const VarSymbol *var) {
  if (otherCaseVars_.contains(var))
    diags_.error(name->loc(), "'" + name->name +
                                  "' is declared in an earlier case of the 'switch'; declare it "
                                  "before the 'switch' to use it in several cases");
}

Expr *Sema::checkUnary(UnaryExpr *unary, const Type *expected, bool wraps) {
  const char *op = opSpelling(unary->op);
  switch (unary->op) {
  case UnaryOp::Neg:
  case UnaryOp::Plus: {
    const Type *hint = expected && expected->nonOptional()->isNumeric() ? expected->nonOptional() : nullptr;
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand))
      unary->operand = checkIntLit(lit, hint, /*negated=*/unary->op == UnaryOp::Neg);
    else
      unary->operand = checkExpr(unary->operand, hint, wraps);
    const Type *type = unary->operand->type;
    if (unary->op == UnaryOp::Plus && (type->isString() || type->isBool())) {
      // `+s` is Number(s), as in JavaScript.
      unary->type = Type::doubleTy();
      break;
    }
    if (!type->isNumeric() && !type->isError()) {
      diags_.error(unary->loc(), llvm::Twine("operator '") + op + "' cannot be applied to type '" +
                                     type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::Not: {
    Flow whenTrue, whenFalse;
    checkBranching(unary, whenTrue, whenFalse);
    flow_ = join(whenTrue, whenFalse);
    break;
  }
  case UnaryOp::BitNot: {
    const Type *hint = expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    unary->operand = checkExpr(unary->operand, hint, /*wraps=*/true);
    const Type *type = unary->operand->type;
    if (type->isFloat()) {
      unary->operand = toInt32(unary->operand);
      type = Type::intTy();
    } else if (!type->isInteger() && !type->isError()) {
      diags_.error(unary->loc(), "operator '~' cannot be applied to type '" + type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::TypeOf:
    // Known from the type, except whether a `T | undefined` holds a value.
    unary->operand = checkExpr(unary->operand);
    unary->type = Type::string();
    break;
  case UnaryOp::NonNull: {
    // `x!`: the program stops if x is undefined, so from here on a variable
    // holds a value.
    unary->operand = checkExpr(unary->operand, expected);
    const Type *type = unary->operand->type;
    if (type->isUndefined()) {
      diags_.error(unary->loc(), "the value is always undefined");
      type = Type::error();
    }
    unary->type = type->nonOptional();
    if (auto *name = dyn_cast<NameExpr>(unary->operand); name && name->var && type->isOptional())
      setDefined(name->var, true);
    break;
  }
  }
  if (unary->type && unary->type->isInteger())
    foldUnary(unary, wraps);
  return unary;
}

Expr *Sema::checkBinary(BinaryExpr *bin, const Type *expected, bool wraps) {
  const char *op = opSpelling(bin->op);
  if (expected)
    expected = expected->nonOptional();
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    Flow whenTrue, whenFalse;
    checkBranching(bin, whenTrue, whenFalse);
    flow_ = join(whenTrue, whenFalse);
    return bin;
  }

  case BinaryOp::Nullish:
    return checkNullish(bin, expected);

  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return checkEquality(bin);

  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    bin->lhs = checkExpr(bin->lhs);
    bin->rhs = checkExpr(bin->rhs);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::boolTy();
    if (l->isError() || r->isError())
      return bin;
    // Strings compare by UTF-16 code units, as in JavaScript.
    if (l->isString() && r->isString())
      return bin;
    if (l->isNumeric() && r->isNumeric()) {
      if (const Type *common = commonNumericType(l, r)) {
        bin->lhs = convert(bin->lhs, common);
        bin->rhs = convert(bin->rhs, common);
        return bin;
      }
    }
    reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }

  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    // Only the low bits of both sides matter: the count is masked.
    const Type *hint = expected && expected->isInteger() ? expected : nullptr;
    bin->lhs = checkExpr(bin->lhs, hint, /*wraps=*/true);
    bin->rhs = checkExpr(bin->rhs, nullptr, /*wraps=*/true);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::error();
    if (l->isError() || r->isError())
      return bin;
    if (!l->isNumeric() || !r->isNumeric()) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    if (l->isFloat()) {
      // JavaScript's `x >>> n` is unsigned, and an int cannot hold it.
      if (bin->op == BinaryOp::UShr) {
        diags_.error(bin->lhs->loc(), "operator '>>>' cannot be applied to type 'double'; "
                                      "convert it to int with 'x | 0' first");
        return bin;
      }
      bin->lhs = toInt32(bin->lhs);
      l = Type::intTy();
    }
    if (r->isFloat())
      bin->rhs = toInt32(bin->rhs);
    // The result has the type of the left side. The shift count is masked to
    // the bit width, so converting it to that type loses nothing.
    bin->rhs = convert(bin->rhs, l);
    bin->type = l;
    foldBinary(bin, wraps);
    return bin;
  }

  default: { // arithmetic and bitwise
    bool bitwise = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
                   bin->op == BinaryOp::BitXor;
    const Type *hint = nullptr;
    if (expected && (bitwise ? expected->isInteger() : expected->isNumeric()))
      hint = expected;
    // The low bits of a sum, difference, product or power depend only on the
    // low bits of the operands; not so for a quotient.
    bool operandsWrap = bitwise || (wraps && bin->op != BinaryOp::Div && bin->op != BinaryOp::Rem);
    bin->lhs = checkExpr(bin->lhs, hint, operandsWrap);
    bin->rhs = checkExpr(bin->rhs, hint, operandsWrap);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::error();
    if (l->isError() || r->isError())
      return bin;
    if (bin->op == BinaryOp::Add && (l->isString() || r->isString())) {
      // As in JavaScript, `+` concatenates when either side is a string.
      bin->lhs = toStringOperand(bin->lhs);
      bin->rhs = toStringOperand(bin->rhs);
      bin->type = Type::string();
      return bin;
    }
    if (bitwise && l->isNumeric() && r->isNumeric() && (l->isFloat() || r->isFloat())) {
      // As in JavaScript, both sides become 32-bit integers first, so `x | 0`
      // drops the fraction of a double exactly the way JavaScript does.
      bin->lhs = toInt32(bin->lhs);
      bin->rhs = toInt32(bin->rhs);
      bin->type = Type::intTy();
      return bin;
    }
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? commonNumericType(l, r) : nullptr;
    if (!common) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    bin->lhs = convert(bin->lhs, common);
    bin->rhs = convert(bin->rhs, common);
    bin->type = common;
    if (!common->isInteger())
      return bin;
    // An integer to a negative power is a fraction (as with `/`, integers
    // stay integers); a negative constant exponent is surely a mistake.
    if (bin->op == BinaryOp::Pow) {
      if (std::optional<int64_t> exponent = intConstant(bin->rhs); exponent && *exponent < 0) {
        diags_.error(bin->rhs->loc(), "'" + common->name() +
                                          "' raised to a negative power is a fraction, which an "
                                          "integer cannot hold; use a double, e.g. '2.0 ** -1'");
        return bin;
      }
    }
    checkDivisor(bin->op, bin->rhs);
    foldBinary(bin, wraps);
    return bin;
  }
  }
}

// `===` and `!==` (`==` and `!=` behave the same, as there is no null). Any
// value can be compared with undefined; `T | undefined` compares with `T`.
Expr *Sema::checkEquality(BinaryExpr *bin) {
  bin->lhs = checkExpr(bin->lhs);
  bin->rhs = checkExpr(bin->rhs);
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bin->type = Type::boolTy();
  if (l->isError() || r->isError())
    return bin;
  if (l->isVoid() || r->isVoid()) {
    reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    return bin;
  }
  if (l->isUndefined() || r->isUndefined())
    return bin;

  const Type *common = comparisonType(l, r);
  if (!common) {
    reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    return bin;
  }
  if (common->nonOptional()->isFunction())
    file_->comparesFunctions = true;
  bin->lhs = convert(bin->lhs, common);
  bin->rhs = convert(bin->rhs, common);
  return bin;
}

// `a ?? b`: b when a is undefined. When a cannot be undefined (for example an
// index Sema proved in range), b is checked but never evaluated.
Expr *Sema::checkNullish(BinaryExpr *bin, const Type *expected) {
  bin->lhs = checkExpr(bin->lhs, expected);
  const Type *l = bin->lhs->type;
  Flow afterLhs = flow_;
  const Type *hint = l->isUndefined() || l->isError() ? expected : l->nonOptional();
  bin->rhs = checkExpr(bin->rhs, hint);
  flow_ = join(afterLhs, flow_); // the right side may not run
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    if (trial_ && l->isError() != r->isError()) // as in checkConditional
      bin->type = l->isError() ? r : l->nonOptional();
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  if (l->isUndefined()) {
    bin->type = r;
    return bin;
  }
  const Type *base = r->isUndefined() ? l->nonOptional() : unify(l->nonOptional(), r->nonOptional());
  if (!base || base->isError()) {
    reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  const Type *result = r->isOptional() || r->isUndefined() ? Type::optional(base) : base;
  bin->lhs = convert(bin->lhs, l->isOptional() ? Type::optional(base) : result);
  bin->rhs = convert(bin->rhs, result);
  bin->type = result;
  return bin;
}

const Type *Sema::checkAssignTarget(Expr *target, bool compound) {
  if (auto *name = dyn_cast<NameExpr>(target)) {
    VarSymbol *var = checkVarTarget(name);
    return var ? var->type : Type::error();
  }

  if (auto *index = dyn_cast<IndexExpr>(target)) {
    index->object = checkExpr(index->object);
    index->index = checkPosition(index->index);
    const Type *type = index->object->type;
    index->type = Type::error();
    if (type->isError())
      return type;
    if (type->isArray()) {
      index->type = type->inner();
      return index->type;
    }
    if (type->isString())
      diags_.error(index->loc(), "strings are immutable; a character cannot be assigned to");
    else if (type->isOptional() && type->inner()->isArray())
      diags_.error(index->object->loc(), "the array may be undefined; check it first");
    else
      diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    return Type::error();
  }

  if (auto *member = dyn_cast<MemberExpr>(target)) {
    member->object = checkExpr(member->object);
    const Type *type = member->object->type;
    member->type = Type::error();
    if (type->isError())
      return type;
    if (type->isArray() && member->name == "length") {
      if (compound) {
        diags_.error(member->loc(), "only '=' can change the length of an array");
        return Type::error();
      }
      member->type = Type::intTy();
      return Type::longTy(); // a new length is computed as long, like positions
    }
    if (type->isString() && member->name == "length")
      diags_.error(member->loc(), "the length of a string cannot be changed");
    else
      diags_.error(member->loc(), "property '" + member->name + "' of type '" + type->name() +
                                      "' cannot be assigned to");
    return Type::error();
  }

  if (isa<ArrayLitExpr>(target))
    diags_.error(target->loc(), "destructuring assignment can only be a statement, e.g. '[a, b] = [b, a]'");
  else
    diags_.error(target->loc(), "only variables, array elements and 'length' can be assigned to");
  return Type::error();
}

VarSymbol *Sema::checkVarTarget(NameExpr *name) {
  ScopeEntry entry = lookup(name->name);
  if (!entry.var) {
    checkName(name); // reports the error
    return nullptr;
  }
  name->var = entry.var;
  name->type = varType(entry.var);
  checkVisible(name, entry.var);
  if (entry.var->isConst)
    diags_.error(name->loc(), "cannot assign to '" + name->name + "' because it is a " +
                                  (entry.var->function ? "function" : "constant"));
  entry.var->reassigned = true;
  noteUse(entry.var, name->loc());
  return entry.var;
}

// After the value of `a[i] = v` is known: the store happens then, so the
// index must be in range at that point.
static const char kWriteOutOfRange[] =
    "cannot prove that the index is within the array (0 <= index <= length); writing further "
    "would leave holes. Check the index first, e.g. 'if (i < a.length)', or use push()";

Expr *Sema::checkAssign(AssignExpr *assign) {
  bool compound = assign->op.has_value();
  BinaryOp op = compound ? *assign->op : BinaryOp::Add;

  if (compound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd))
    return checkLogicalAssign(assign);

  if (compound && op == BinaryOp::Nullish) {
    // `x ??= v`: assigns only if x is undefined.
    auto *name = dyn_cast<NameExpr>(assign->target);
    VarSymbol *var = name ? checkVarTarget(name) : nullptr;
    if (!name)
      diags_.error(assign->target->loc(), "only variables can be the target of '??='");
    const Type *type = var ? var->type : Type::error();
    assign->type = type;
    if (!type->isOptional() && !type->isError()) {
      diags_.error(assign->loc(), "'??=' needs a variable that may be undefined; '" + name->name +
                                      "' has type '" + type->name() + "'");
      type = Type::error();
    }
    Flow before = flow_;
    assign->value = checkExpr(assign->value, type->isError() ? nullptr : type->inner());
    const Type *valueType = assign->value->type;
    flow_ = join(before, flow_);
    if (type->isError())
      return assign;
    bool definite = !valueType->isOptional() && !valueType->isUndefined();
    assign->value = coerce(assign->value, definite ? type->inner() : type);
    if (var) {
      if (definite)
        setDefined(var, true);
      assign->type = definite ? type->inner() : type;
    }
    return assign;
  }

  const Type *type = checkAssignTarget(assign->target, compound);
  assign->type = type;
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *name = dyn_cast<NameExpr>(assign->target);
  const VarSymbol *var = name ? name->var : nullptr;
  // `a[i] op= v` reads the element before v is evaluated.
  bool reportedRange = false;
  if (index && compound && index->object->type->isArray() &&
      !provesInBounds(index->object, index->index, /*allowEnd=*/false)) {
    diags_.error(index->loc(), "cannot prove that the index is within the array; check it first, "
                               "e.g. 'if (i < a.length)'");
    reportedRange = true;
  }

  if (!compound) {
    if (auto *fe = dyn_cast<FunctionExpr>(assign->value); fe && name && fe->fn->name.empty())
      fe->fn->name = name->name; // as JavaScript names it
    Expr *value = checkExpr(assign->value, type->isError() ? nullptr : type);
    assign->value = coerce(value, type);
    if (var)
      assigned(var, value);
  } else {
    std::string spelling = std::string(opSpelling(op)) + "=";
    if (type->isString()) {
      assign->value = checkExpr(assign->value);
      if (op != BinaryOp::Add) {
        if (!assign->value->type->isError())
          reportOperandTypes(assign->loc(), spelling.c_str(), type, assign->value->type);
      } else {
        assign->value = toStringOperand(assign->value);
      }
      if (var)
        flow_.facts.forget(var);
    } else {
      bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
      bool integerOnly =
          shift || op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
      assign->value = checkExpr(assign->value, shift ? nullptr : type, /*wraps=*/integerOnly);
      const Type *valueType = assign->value->type;
      if (!type->isError() && !valueType->isError()) {
        if (integerOnly && valueType->isFloat())
          assign->value = toInt32(assign->value); // as with `x | v`
        if (integerOnly && type->isFloat() && valueType->isNumeric() && op != BinaryOp::UShr) {
          // `x |= v` on a double works on 32-bit integers, as in JavaScript;
          // the result goes back into x as a double.
          assign->value = toInt32(assign->value);
        } else if (integerOnly ? !type->isInteger() : !type->isNumeric()) {
          reportOperandTypes(assign->loc(), spelling.c_str(), type, valueType);
        } else {
          // `x op= v` must produce a value of x's type, so v converts to it.
          assign->value = shift ? convert(assign->value, type) : coerce(assign->value, type);
          if (type->isInteger())
            checkDivisor(op, assign->value);
        }
      }
      if (var) {
        std::optional<Linear> delta = linear(assign->value);
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && delta && delta->term.isZero())
          incremented(var, op == BinaryOp::Add ? delta->offset : -delta->offset);
        else
          flow_.facts.forget(var);
      }
    }
  }

  if (index && index->object->type->isArray()) {
    // The store happens after the value is evaluated, so prove it here.
    bool inRange = provesInBounds(index->object, index->index, /*allowEnd=*/false);
    if (!inRange && !reportedRange &&
        (compound || !provesInBounds(index->object, index->index, /*allowEnd=*/true)))
      diags_.error(index->loc(), kWriteOutOfRange);
    index->mayAppend = !inRange;
    if (!inRange)
      arrayResized(index->object->type, /*grew=*/true, /*shrank=*/false);
  }
  if (member && member->object->type->isArray()) {
    if (std::optional<Linear> length = linear(assign->value);
        length && length->term.isZero() && length->offset < 0)
      diags_.error(assign->value->loc(), "an array length cannot be negative");
    arrayResized(member->object->type, /*grew=*/true, /*shrank=*/true);
    if (std::optional<Linear> self = lengthOf(member->object); self && !self->term.isZero())
      if (std::optional<Linear> length = linear(assign->value))
        flow_.facts.addEqual(self->term, length->term, length->offset);
  }
  return assign;
}

// `x ||= v` is `x || (x = v)` and `x &&= v` is `x && (x = v)`: v is evaluated
// and assigned only when x does not decide the result. x is read twice, so it
// must be a variable or an element at a simple index.
Expr *Sema::checkLogicalAssign(AssignExpr *assign) {
  const char *op = assign->op == BinaryOp::LogOr ? "||=" : "&&=";
  auto simple = [](const Expr *e) { return isa<NameExpr>(e) || isa<IntLitExpr>(e); };
  auto copy = [&](const Expr *e) -> Expr * {
    if (auto *name = dyn_cast<NameExpr>(e))
      return ctx_.make<NameExpr>(name->loc(), name->name);
    return ctx_.make<IntLitExpr>(e->loc(), cast<IntLitExpr>(e)->value);
  };
  Expr *read = nullptr;
  if (auto *name = dyn_cast<NameExpr>(assign->target)) {
    if (VarSymbol *var = lookup(name->name).var; var && !var->type->isBool() && !var->type->isError()) {
      diags_.error(assign->loc(), llvm::Twine("'") + op + "' needs a boolean variable; '" + name->name +
                                      "' has type '" + var->type->name() + "'" +
                                      (var->type->isOptional() ? "; for undefined use '??='" : ""));
      assign->value = checkExpr(assign->value);
      assign->type = Type::error();
      return assign;
    }
    read = copy(name);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target);
             index && !index->optional && simple(index->object) && simple(index->index)) {
    read = ctx_.make<IndexExpr>(index->loc(), copy(index->object), copy(index->index));
  } else {
    diags_.error(assign->target->loc(), llvm::Twine("'") + op +
                                            "' needs a variable or an array element at a simple "
                                            "index, such as 'a[i]'");
    assign->value = checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set = ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  return checkExpr(ctx_.make<BinaryExpr>(assign->loc(), *assign->op, read, set));
}

Expr *Sema::checkUpdate(UpdateExpr *update) {
  update->type = Type::error();
  const Type *type = checkAssignTarget(update->target, /*compound=*/true);
  if (type->isError())
    return update;
  if (!type->isNumeric()) {
    diags_.error(update->loc(), llvm::Twine("operator '") + (update->increment ? "++" : "--") +
                                    "' cannot be applied to type '" + type->name() + "'");
    return update;
  }
  update->type = type;
  if (auto *name = dyn_cast<NameExpr>(update->target)) {
    if (name->var)
      incremented(name->var, update->increment ? 1 : -1);
  } else if (auto *index = dyn_cast<IndexExpr>(update->target)) {
    if (!provesInBounds(index->object, index->index, /*allowEnd=*/false))
      diags_.error(index->loc(), "cannot prove that the index is within the array; check it "
                                 "first, e.g. 'if (i < a.length)'");
  }
  return update;
}

Expr *Sema::checkCall(CallExpr *call, const Type *expected) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };

  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !call->optional) {
    if (isGlobalName(member->object, "console")) {
      if (rejectSpread(call))
        return call;
      if (member->name == "log")
        return checkConsoleLog(call);
      diags_.error(member->loc(), "'console." + member->name +
                                      "' is not supported yet; only 'console.log' is");
      checkArgsLoosely();
      return call;
    }
    if (isGlobalName(member->object, "Math"))
      return checkMathCall(call, member, expected);
    if (isGlobalName(member->object, "String") || isGlobalName(member->object, "Number")) {
      if (rejectSpread(call))
        return call;
      return checkStaticCall(call, member, expected);
    }
    member->object = checkExpr(member->object);
    if (member->optional)
      member->object = chainLink(member->object);
    const Type *receiver = member->object->type;
    if (!(receiver->isArray() && member->name == "push") && rejectSpread(call))
      return call;
    if (receiver->isString())
      return checkStringMethod(call, member);
    if (receiver->isArray())
      return checkArrayMethod(call, member);
    if (receiver->isNumeric())
      return checkNumberMethod(call, member);
    if (receiver->isBool() && member->name == "toString" && call->args.empty()) {
      // `x.toString()` is `String(x)`.
      call->args.push_back(toStringOperand(member->object));
      call->builtin = Builtin::StringOf;
      call->type = Type::string();
      return call;
    }
    if (receiver->isOptional())
      diags_.error(member->object->loc(), "the value may be undefined; check it before calling '" +
                                              member->name + "'");
    else if (receiver->isFunction() &&
             (member->name == "call" || member->name == "apply" || member->name == "bind"))
      diags_.error(member->loc(), "'" + member->name + "' is not supported yet: it sets 'this', "
                                                         "which exists only in classes; call the "
                                                         "function directly");
    else if (!receiver->isError())
      diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                      receiver->name() + "'");
    checkArgsLoosely();
    return call;
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && !call->optional) {
    if (Expr *builtin = checkGlobalCall(call, name, expected))
      return builtin;
    ScopeEntry entry = lookup(name->name);
    if (entry.func && !entry.var)
      return checkDirectCall(call, entry.func, name->loc()); // a top-level function
    // A function declared in a block, or a constant arrow: called directly,
    // with the environment its variable holds.
    VarSymbol *var = entry.var;
    FunctionDecl *known = var ? (var->function ? var->function : var->isConst ? var->lambda : nullptr) : nullptr;
    if (known && known->sym) {
      name->var = var;
      noteUse(var, name->loc());
      name->type = varType(var);
      return checkDirectCall(call, known->sym, name->loc());
    }
    if (!entry.var && !entry.func) {
      if (!checkName(name)->type->isError()) // NaN, Infinity
        diags_.error(name->loc(), "'" + name->name + "' is not a function");
      checkArgsLoosely();
      return call;
    }
  }

  // A call through a function value: `f(x)` with a variable f, `fs[i](x)`,
  // `make()(x)`, `f?.(x)`.
  auto *calleeName = dyn_cast<NameExpr>(call->callee);
  call->callee = checkExpr(call->callee);
  if (call->optional)
    call->callee = chainLink(call->callee);
  const Type *type = call->callee->type;
  if (type->isFunction())
    return checkValueCall(call, type);
  if (type->isOptional() && type->inner()->isFunction())
    diags_.error(call->callee->loc(), "the function may be undefined; check it first, or call it with "
                                      "'?.()', which gives undefined then");
  else if (calleeName && !type->isError())
    diags_.error(call->callee->loc(), "'" + calleeName->name + "' is not a function; it has type '" +
                                          type->name() + "'");
  else if (!type->isError())
    diags_.error(call->callee->loc(), "a value of type '" + type->name() + "' cannot be called");
  checkArgsLoosely();
  return call;
}

Expr *Sema::checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc) {
  resolveParams(func);
  call->func = func;
  call->type = returnType(func);
  return checkArguments(call, func->params, func->minArgs, func->hasRest, loc);
}

Expr *Sema::checkValueCall(CallExpr *call, const Type *type) {
  call->type = type->ret();
  return checkArguments(call, type->params(), type->minArgs(), type->hasRest(), call->callee->loc());
}

Expr *Sema::checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs,
                           bool hasRest, llvm::SMLoc loc) {
  size_t fixed = params.size() - (hasRest ? 1 : 0);
  for (size_t i = 0; i < call->args.size() && i < (hasRest ? fixed : call->args.size()); ++i)
    if (call->spreads(i) && rejectSpread(call))
      return call;
  if (!checkArity(call, loc, static_cast<unsigned>(minArgs),
                  hasRest ? ~0u : static_cast<unsigned>(params.size())))
    return call;
  if (hasRest) {
    // The arguments for the rest parameter become one array, as `f(1, ...a)`
    // passes `[1, ...a]`.
    llvm::SMLoc restLoc = call->args.size() > fixed ? call->args[fixed]->loc() : call->loc();
    auto *rest = ctx_.make<ArrayLitExpr>(restLoc);
    for (size_t i = fixed; i < call->args.size(); ++i)
      rest->elements.push_back({call->args[i], call->spreads(i)});
    call->args.resize(std::min(call->args.size(), fixed));
    while (call->args.size() < fixed) // optional parameters left out
      call->args.push_back(ctx_.make<UndefinedExpr>(call->loc()));
    call->args.push_back(rest);
    call->spread.clear();
  }
  // Parameters left out get undefined, and with it their default values.
  for (size_t i = 0; i < call->args.size(); ++i)
    call->args[i] = checkArg(call->args[i], params[i]);
  calledFunction();
  return call;
}

bool Sema::checkArity(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max) {
  size_t count = call->args.size();
  if (count >= min && count <= max)
    return true;
  diags_.error(loc, "expected " + argumentCount(min, max) + ", but got " + llvm::Twine(count));
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return false;
}

bool Sema::rejectSpread(CallExpr *call) {
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (!call->spreads(i))
      continue;
    diags_.error(call->args[i]->loc(), "a spread argument can only be passed to a rest parameter, "
                                       "Math.min, Math.max or push");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    call->type = Type::error();
    return true;
  }
  return false;
}

Expr *Sema::checkArg(Expr *arg, const Type *to) {
  return coerce(checkExpr(arg, to->isError() ? nullptr : to), to);
}

Expr *Sema::checkConsoleLog(CallExpr *call) {
  call->builtin = Builtin::ConsoleLog;
  call->type = Type::voidTy();
  for (Expr *&arg : call->args) {
    arg = checkExpr(arg);
    if (arg->type->isVoid())
      diags_.error(arg->loc(), "an expression of type 'void' cannot be printed");
  }
  return call;
}

bool Sema::isGlobalName(const Expr *expr, llvm::StringRef name) const {
  auto *n = dyn_cast<NameExpr>(expr);
  if (!n || n->name != name)
    return false;
  ScopeEntry shadow = lookup(name);
  return !shadow.var && !shadow.func;
}

Expr *Sema::toStringOperand(Expr *expr) {
  const Type *type = expr->type;
  if (type->isString() || type->isError())
    return expr;
  if (type->isVoid()) {
    diags_.error(expr->loc(), "an expression of type 'void' cannot be converted to a string");
    return expr;
  }
  if (holdsFunctions(type)) {
    // JavaScript would give the source code of the function.
    diags_.error(expr->loc(), "a function cannot be converted to a string; print it with "
                              "console.log, or use its name");
    return expr;
  }
  // Numbers, booleans, undefined, arrays (joined with commas) and optionals of
  // those convert as in JavaScript.
  return convert(expr, Type::string());
}

Expr *Sema::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError() ? expr : convert(expr, Type::intTy());
}

Expr *Sema::checkTemplate(TemplateExpr *tmpl) {
  for (Expr *&e : tmpl->exprs)
    e = toStringOperand(checkExpr(e));
  tmpl->type = Type::string();
  return tmpl;
}

Expr *Sema::checkMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  if (auto *name = dyn_cast<NameExpr>(member->object))
    if (EnumDecl *decl = lookup(name->name).enumDecl)
      return checkEnumMember(decl, member, expected);
  for (llvm::StringRef global : {"console", "Math", "String", "Number"}) {
    if (isGlobalName(member->object, global)) {
      if (Expr *constant = globalConstant(global, member->name, member->loc()))
        return constant;
      diags_.error(member->loc(), "'" + global + "." + member->name +
                                      "' can be used as a value only where a function type is "
                                      "expected, e.g. as a callback");
      return member;
    }
  }
  member->object = checkExpr(member->object);
  if (member->optional)
    member->object = chainLink(member->object);
  const Type *type = member->object->type;
  if (type->isError())
    return member;
  if ((type->isString() || type->isArray()) && member->name == "length") {
    member->type = Type::intTy();
    return member;
  }
  if (type->isOptional())
    diags_.error(member->object->loc(), "the value may be undefined; check it before reading '" +
                                            member->name + "'");
  else if (type->isArray() || type->isString())
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                    type->name() + "'; methods must be called");
  else
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                    type->name() + "'");
  return member;
}

Expr *Sema::checkPosition(Expr *expr) {
  expr = checkExpr(expr, Type::longTy());
  const Type *type = expr->type;
  if (type->isError())
    return expr;
  if (!type->isInteger()) {
    std::string message = "an index or position must be an integer, found '" + type->name().str() + "'";
    if (type->isFloat())
      message += "; round it with Math.floor(x) or drop the fraction with 'x | 0'";
    else if (type->isOptional())
      message += "; the value may be undefined: check it first or give a default with '??'";
    diags_.error(expr->loc(), message);
    return expr;
  }
  return convert(expr, Type::longTy());
}

Expr *Sema::checkIndex(IndexExpr *index) {
  if (auto *name = dyn_cast<NameExpr>(index->object))
    if (EnumDecl *decl = lookup(name->name).enumDecl)
      return checkEnumIndex(decl, index);
  index->object = checkExpr(index->object);
  if (index->optional)
    index->object = chainLink(index->object);
  index->index = checkPosition(index->index);
  index->type = Type::error();
  const Type *type = index->object->type;
  if (type->isError() || index->index->type->isError())
    return index;
  const Type *element = type->isString() ? type : type->isArray() ? type->inner() : nullptr;
  if (!element) {
    if (type->isOptional() && (type->inner()->isArray() || type->inner()->isString()))
      diags_.error(index->object->loc(), "the value may be undefined; check it before indexing");
    else
      diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    return index;
  }
  // Proved in range: a plain read. Otherwise JavaScript yields undefined.
  bool inRange = provesInBounds(index->object, index->index, /*allowEnd=*/false);
  index->mayBeOutOfRange = !inRange;
  index->type = inRange ? element : Type::optional(element);
  return index;
}

Expr *Sema::checkConditional(ConditionalExpr *cond, const Type *expected) {
  Flow whenTrue, whenFalse;
  cond->cond = checkCondition(cond->cond, whenTrue, whenFalse);
  flow_ = whenTrue;
  cond->thenExpr = checkExpr(cond->thenExpr, expected);
  Flow afterThen = flow_;
  flow_ = whenFalse;
  cond->elseExpr = checkExpr(cond->elseExpr, expected);
  flow_ = join(afterThen, flow_);

  const Type *a = cond->thenExpr->type, *b = cond->elseExpr->type;
  cond->type = Type::error();
  if (a->isError() || b->isError()) {
    // A call in a cycle may have no type yet (see inferCycle): the other
    // branch tells.
    if (trial_ && a->isError() != b->isError())
      cond->type = a->isError() ? b : a;
    return cond;
  }
  if (a->isVoid() && b->isVoid()) {
    cond->type = a;
    return cond;
  }
  const Type *common = unify(a, b);
  if (!common) {
    diags_.error(cond->loc(), "the branches of '?:' have incompatible types '" + a->name() +
                                  "' and '" + b->name() + "'");
    return cond;
  }
  cond->thenExpr = convert(cond->thenExpr, common);
  cond->elseExpr = convert(cond->elseExpr, common);
  cond->type = common;
  return cond;
}

// `x as T` is not part of the language: in TypeScript it changes nothing at
// run time, so a conversion written with it would behave differently under
// tsc. The error says what to write instead.
Expr *Sema::checkCast(CastExpr *cast) {
  const Type *to = resolveType(*cast->target, /*allowVoid=*/false);
  // An array literal gets the type it is cast to, so that `[] as int[]`
  // reports only this error.
  bool arrayLiteral = isa<ArrayLitExpr>(cast->operand);
  cast->operand = checkExpr(cast->operand, arrayLiteral && !to->isError() ? to : nullptr);
  const Type *from = cast->operand->type;
  cast->type = to;
  std::string message = "'as' is not supported";
  if (from->isError() || to->isError())
    ;
  else if (arrayLiteral)
    message += "; declare the type of the variable instead, e.g. 'let x: " + to->name().str() +
               " = ...'";
  else if (from == to || isImplicitlyConvertible(from, to))
    message += "; the value converts to '" + to->name().str() + "' by itself, so remove 'as'";
  else if (from->isOptional() && isImplicitlyConvertible(from->inner(), to))
    message += "; the value may be undefined: check it first (e.g. 'x !== undefined') or give a "
               "default with '??'";
  else if (from->isFloat() && to->isInt())
    message += "; drop the fraction with 'x | 0', or round with Math.round, Math.floor, Math.ceil "
               "or Math.trunc";
  else if (from->isFloat() && to->isInteger())
    message += "; round with Math.round, Math.floor, Math.ceil or Math.trunc where a '" +
               to->name().str() + "' is expected";
  else if (from->isInteger() && to->isInteger())
    message += "; Math.trunc(x) where an 'int' is expected converts it and checks that it fits";
  else
    message += ", and type '" + from->name().str() + "' cannot become '" + to->name().str() + "'";
  diags_.error(cast->loc(), message);
  return cast;
}

} // namespace tz
