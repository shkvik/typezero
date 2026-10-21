#include "sema/Sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

void Sema::check(SourceFile &file) {
  pushScope(); // functions and globals

  // Functions are visible in the whole file, as in JavaScript.
  for (Stmt *item : file.items)
    if (auto *fn = dyn_cast<FunctionDecl>(item))
      declareFunction(fn);

  // Top-level code runs in order; its `let` and `const` become globals.
  for (Stmt *item : file.items)
    if (!isa<FunctionDecl>(item))
      checkStmt(item);

  // Bodies come last so that functions can use globals declared after them.
  for (Stmt *item : file.items)
    if (auto *fn = dyn_cast<FunctionDecl>(item))
      checkFunctionBody(fn);

  popScope();
}

//===----------------------------------------------------------------------===//
// Scopes and types
//===----------------------------------------------------------------------===//

void Sema::declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry) {
  auto [it, inserted] = scopes_.back().try_emplace(name, entry);
  if (inserted)
    return;
  diags_.error(loc, "duplicate declaration of '" + name + "'");
  const ScopeEntry &previous = it->second;
  diags_.note(previous.var ? previous.var->loc : previous.func->loc, "previously declared here");
}

Sema::ScopeEntry Sema::lookup(llvm::StringRef name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end())
      return found->second;
  }
  return {};
}

const Type *Sema::resolveType(const TypeRef &ref, bool allowVoid) {
  if (ref.name.empty())
    return Type::error(); // the parser already reported it
  const Type *type = Type::fromName(ref.name);
  if (!type) {
    diags_.error(ref.loc, "cannot find type '" + ref.name + "'");
    return Type::error();
  }
  if (type->isString()) {
    diags_.error(ref.loc, "the 'string' type is not supported yet");
    return Type::error();
  }
  if (type->isVoid() && !allowVoid) {
    diags_.error(ref.loc, "'void' can only be used as a return type");
    return Type::error();
  }
  return type;
}

bool Sema::isImplicitlyConvertible(const Type *from, const Type *to) {
  return from == to ||
         (from == Type::intTy() && (to == Type::longTy() || to == Type::doubleTy()));
}

const Type *Sema::commonNumericType(const Type *a, const Type *b) {
  if (isImplicitlyConvertible(a, b))
    return b;
  if (isImplicitlyConvertible(b, a))
    return a;
  return nullptr;
}

Expr *Sema::convert(Expr *expr, const Type *to) {
  if (expr->type == to || expr->type->isError() || to->isError())
    return expr;
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

  std::string message =
      ("type '" + from->name() + "' is not assignable to type '" + to->name() + "'").str();
  if (from->isNumeric() && to->isNumeric())
    message += ("; convert it explicitly with 'as " + to->name() + "'").str();
  diags_.error(expr->loc(), message);
  return expr;
}

void Sema::reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" +
                         lhs->name() + "' and '" + rhs->name() + "'")
                            .str();
  if (lhs->isNumeric() && rhs->isNumeric())
    message += "; convert one side explicitly with 'as'";
  diags_.error(loc, message);
}

//===----------------------------------------------------------------------===//
// Declarations and statements
//===----------------------------------------------------------------------===//

void Sema::declareFunction(FunctionDecl *fn) {
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  for (const Param &param : fn->params) {
    if (param.type) {
      sym->params.push_back(resolveType(*param.type, /*allowVoid=*/false));
    } else {
      diags_.error(param.loc, "parameter '" + param.name + "' needs a type annotation");
      sym->params.push_back(Type::error());
    }
  }
  // Unlike TypeScript, a missing return type means `void` rather than inferred.
  sym->ret = fn->returnType ? resolveType(*fn->returnType, /*allowVoid=*/true) : Type::voidTy();
  fn->sym = sym;
  if (!fn->name.empty())
    declare(fn->name, fn->loc(), {nullptr, sym});
}

void Sema::checkFunctionBody(FunctionDecl *fn) {
  FuncSymbol *sym = fn->sym;
  currentFunction_ = sym;
  pushScope();
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    VarSymbol *var = ctx_.newVar();
    var->name = param.name;
    var->loc = param.loc;
    var->type = sym->params[i];
    param.sym = var;
    declare(param.name, param.loc, {var, nullptr});
  }
  // The parameters and the outermost block of the body share one scope.
  for (Stmt *stmt : fn->body->body)
    checkStmt(stmt);
  popScope();
  currentFunction_ = nullptr;

  if (!sym->ret->isVoid() && !sym->ret->isError() && !alwaysReturns(fn->body))
    diags_.error(fn->loc(), "function '" + fn->name + "' must return a value of type '" +
                                sym->ret->name() + "' on every path");
}

void Sema::checkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    pushScope();
    for (Stmt *s : cast<BlockStmt>(stmt)->body)
      checkStmt(s);
    popScope();
    break;
  case Stmt::Kind::VarDecl:
    checkVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::Function:
    // check() handles top-level functions; this one is nested.
    diags_.error(stmt->loc(), "functions can only be declared at the top level for now");
    break;
  case Stmt::Kind::Expr: {
    auto *s = cast<ExprStmt>(stmt);
    s->expr = checkExpr(s->expr);
    break;
  }
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    s->cond = checkCondition(s->cond);
    checkStmt(s->thenStmt);
    if (s->elseStmt)
      checkStmt(s->elseStmt);
    break;
  }
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    s->cond = checkCondition(s->cond);
    checkLoopBody(s->body);
    break;
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    checkLoopBody(s->body);
    s->cond = checkCondition(s->cond);
    break;
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    pushScope(); // variables declared in the loop header
    if (s->init)
      checkStmt(s->init);
    if (s->cond)
      s->cond = checkCondition(s->cond);
    if (s->step)
      s->step = checkExpr(s->step);
    checkLoopBody(s->body);
    popScope();
    break;
  }
  case Stmt::Kind::Return:
    checkReturn(cast<ReturnStmt>(stmt));
    break;
  case Stmt::Kind::Break:
    if (loopDepth_ == 0)
      diags_.error(stmt->loc(), "'break' can only be used inside a loop");
    break;
  case Stmt::Kind::Continue:
    if (loopDepth_ == 0)
      diags_.error(stmt->loc(), "'continue' can only be used inside a loop");
    break;
  case Stmt::Kind::Empty:
    break;
  }
}

void Sema::checkLoopBody(Stmt *body) {
  ++loopDepth_;
  checkStmt(body);
  --loopDepth_;
}

void Sema::checkVarDecl(VarDeclStmt *decl) {
  bool isGlobal = scopes_.size() == 1;
  for (VarDeclarator &d : decl->decls) {
    const Type *declared = d.typeAnn ? resolveType(*d.typeAnn, /*allowVoid=*/false) : nullptr;
    const Type *type = declared;
    if (d.init) {
      d.init = checkExpr(d.init, declared);
      if (declared) {
        d.init = coerce(d.init, declared);
      } else {
        type = d.init->type;
        if (type->isVoid()) {
          diags_.error(d.init->loc(), "an expression of type 'void' cannot initialize a variable");
          type = Type::error();
        } else if (type->isString()) {
          diags_.error(d.loc, "string variables are not supported yet");
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

    VarSymbol *var = ctx_.newVar();
    var->name = d.name;
    var->loc = d.loc;
    var->type = type;
    var->isConst = decl->isConst;
    var->isGlobal = isGlobal;
    d.sym = var;
    declare(d.name, d.loc, {var, nullptr});
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
  if (!ret->value) {
    if (!fn->ret->isVoid() && !fn->ret->isError())
      diags_.error(ret->loc(), "function '" + fn->name + "' must return a value of type '" +
                                   fn->ret->name() + "'");
    return;
  }

  ret->value = checkExpr(ret->value, fn->ret->isVoid() ? nullptr : fn->ret);
  if (fn->ret->isVoid()) {
    // `return f()` with a void `f` is fine, as in TypeScript.
    const Type *type = ret->value->type;
    if (!type->isVoid() && !type->isError())
      diags_.error(ret->value->loc(), "function '" + fn->name +
                                          "' has no return type; declare one, e.g. '): " +
                                          type->name() + "'");
    return;
  }
  ret->value = coerce(ret->value, fn->ret);
}

Expr *Sema::checkCondition(Expr *cond) {
  cond = checkExpr(cond, Type::boolTy());
  if (!cond->type->isBool() && !cond->type->isError())
    diags_.error(cond->loc(), "condition must be of type 'boolean', found '" + cond->type->name() + "'");
  return cond;
}

static bool isTrueLiteral(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<BoolLitExpr>(expr);
  return lit && lit->value;
}

// Conservative: true only if control can never reach the end of `stmt`.
bool Sema::alwaysReturns(const Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
    return true;
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, alwaysReturns);
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    return s->elseStmt && alwaysReturns(s->thenStmt) && alwaysReturns(s->elseStmt);
  }
  // An endless loop never falls through unless it breaks.
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !containsBreak(s->body);
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !containsBreak(s->body);
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    return (!s->cond || isTrueLiteral(s->cond)) && !containsBreak(s->body);
  }
  default:
    return false;
  }
}

// Whether `stmt` contains a `break` that leaves the loop enclosing it. Nested
// loops are skipped: their breaks are their own.
bool Sema::containsBreak(const Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Break:
    return true;
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, containsBreak);
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    return containsBreak(s->thenStmt) || (s->elseStmt && containsBreak(s->elseStmt));
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

Expr *Sema::checkExpr(Expr *expr, const Type *expected) {
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
  case Expr::Kind::Name:
    return checkName(cast<NameExpr>(expr));
  case Expr::Kind::Member:
    diags_.error(expr->loc(), "property access is not supported yet");
    expr->type = Type::error();
    return expr;
  case Expr::Kind::Unary:
    return checkUnary(cast<UnaryExpr>(expr), expected);
  case Expr::Kind::Binary:
    return checkBinary(cast<BinaryExpr>(expr), expected);
  case Expr::Kind::Assign:
    return checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return checkCall(cast<CallExpr>(expr));
  case Expr::Kind::Conditional:
    return checkConditional(cast<ConditionalExpr>(expr), expected);
  case Expr::Kind::Cast:
    return checkCast(cast<CastExpr>(expr));
  }
  llvm_unreachable("unknown expression kind");
}

// An integer literal takes the numeric type the context expects; otherwise
// int, or long if it does not fit.
Expr *Sema::checkIntLit(IntLitExpr *lit, const Type *expected, bool negated) {
  auto fits = [&](unsigned bits) {
    uint64_t limit = uint64_t(1) << (bits - 1);
    return negated ? lit->value <= limit : lit->value < limit;
  };

  if (expected && expected->isNumeric()) {
    lit->type = expected;
    if (expected->isInteger() && !fits(expected->bitWidth()))
      diags_.error(lit->loc(), "integer literal does not fit in type '" + expected->name() + "'");
    return lit;
  }
  if (fits(32)) {
    lit->type = Type::intTy();
  } else if (fits(64)) {
    lit->type = Type::longTy();
  } else {
    diags_.error(lit->loc(), "integer literal does not fit in 64 bits");
    lit->type = Type::error();
  }
  return lit;
}

Expr *Sema::checkName(NameExpr *name) {
  ScopeEntry entry = lookup(name->name);
  if (entry.var) {
    name->var = entry.var;
    name->type = entry.var->type;
    return name;
  }
  if (entry.func)
    diags_.error(name->loc(), "functions cannot be used as values yet");
  else
    diags_.error(name->loc(), "cannot find name '" + name->name + "'");
  name->type = Type::error();
  return name;
}

Expr *Sema::checkUnary(UnaryExpr *unary, const Type *expected) {
  const char *op = opSpelling(unary->op);
  switch (unary->op) {
  case UnaryOp::Neg:
  case UnaryOp::Plus: {
    const Type *hint = expected && expected->isNumeric() ? expected : nullptr;
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand))
      unary->operand = checkIntLit(lit, hint, /*negated=*/unary->op == UnaryOp::Neg);
    else
      unary->operand = checkExpr(unary->operand, hint);
    const Type *type = unary->operand->type;
    if (!type->isNumeric() && !type->isError()) {
      diags_.error(unary->loc(), llvm::Twine("operator '") + op + "' cannot be applied to type '" +
                                     type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::Not: {
    unary->operand = checkExpr(unary->operand, Type::boolTy());
    const Type *type = unary->operand->type;
    if (!type->isBool() && !type->isError())
      diags_.error(unary->loc(), "operator '!' cannot be applied to type '" + type->name() + "'");
    unary->type = Type::boolTy();
    break;
  }
  case UnaryOp::BitNot: {
    const Type *hint = expected && expected->isInteger() ? expected : nullptr;
    unary->operand = checkExpr(unary->operand, hint);
    const Type *type = unary->operand->type;
    if (!type->isInteger() && !type->isError()) {
      diags_.error(unary->loc(), "operator '~' cannot be applied to type '" + type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  }
  return unary;
}

Expr *Sema::checkBinary(BinaryExpr *bin, const Type *expected) {
  const char *op = opSpelling(bin->op);
  switch (bin->op) {
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    bin->lhs = checkExpr(bin->lhs, Type::boolTy());
    bin->rhs = checkExpr(bin->rhs, Type::boolTy());
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (!l->isError() && !r->isError() && !(l->isBool() && r->isBool()))
      reportOperandTypes(bin->loc(), op, l, r);
    bin->type = Type::boolTy();
    return bin;
  }

  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne: {
    bin->lhs = checkExpr(bin->lhs);
    bin->rhs = checkExpr(bin->rhs);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::boolTy();
    if (l->isError() || r->isError())
      return bin;
    bool equality = bin->op == BinaryOp::Eq || bin->op == BinaryOp::Ne;
    if (equality && l->isBool() && r->isBool())
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
    const Type *hint = expected && expected->isInteger() ? expected : nullptr;
    bin->lhs = checkExpr(bin->lhs, hint);
    bin->rhs = checkExpr(bin->rhs);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::error();
    if (l->isError() || r->isError())
      return bin;
    if (!l->isInteger() || !r->isInteger()) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    // The result has the type of the left side. The shift count is masked to
    // the bit width, so converting it to that type loses nothing.
    bin->rhs = convert(bin->rhs, l);
    bin->type = l;
    return bin;
  }

  default: { // arithmetic and bitwise
    bool bitwise = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
                   bin->op == BinaryOp::BitXor;
    const Type *hint = nullptr;
    if (expected && (bitwise ? expected->isInteger() : expected->isNumeric()))
      hint = expected;
    bin->lhs = checkExpr(bin->lhs, hint);
    bin->rhs = checkExpr(bin->rhs, hint);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::error();
    if (l->isError() || r->isError())
      return bin;
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? commonNumericType(l, r) : nullptr;
    if (!common) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    bin->lhs = convert(bin->lhs, common);
    bin->rhs = convert(bin->rhs, common);
    bin->type = common;
    return bin;
  }
  }
}

VarSymbol *Sema::checkAssignTarget(Expr *target) {
  auto *name = dyn_cast<NameExpr>(target);
  if (!name) {
    diags_.error(target->loc(), "only variables can be assigned to for now");
    return nullptr;
  }
  checkName(name);
  if (name->var && name->var->isConst)
    diags_.error(name->loc(), "cannot assign to '" + name->name + "' because it is a constant");
  return name->var;
}

Expr *Sema::checkAssign(AssignExpr *assign) {
  VarSymbol *var = checkAssignTarget(assign->target);
  const Type *type = var ? var->type : Type::error();
  assign->type = type;

  if (!assign->op) {
    assign->value = coerce(checkExpr(assign->value, type), type);
    return assign;
  }

  BinaryOp op = *assign->op;
  std::string spelling = std::string(opSpelling(op)) + "=";
  bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
  bool integerOnly = shift || op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;

  assign->value = checkExpr(assign->value, shift ? nullptr : type);
  const Type *valueType = assign->value->type;
  if (type->isError() || valueType->isError())
    return assign;

  bool targetOk = integerOnly ? type->isInteger() : type->isNumeric();
  if (!targetOk || (shift && !valueType->isInteger())) {
    reportOperandTypes(assign->loc(), spelling.c_str(), type, valueType);
    return assign;
  }
  // `x op= v` must produce a value of x's type, so v converts to it.
  assign->value = shift ? convert(assign->value, type) : coerce(assign->value, type);
  return assign;
}

Expr *Sema::checkUpdate(UpdateExpr *update) {
  VarSymbol *var = checkAssignTarget(update->target);
  update->type = Type::error();
  if (!var || var->type->isError())
    return update;
  if (!var->type->isNumeric()) {
    diags_.error(update->loc(), llvm::Twine("operator '") + (update->increment ? "++" : "--") +
                                    "' cannot be applied to type '" + var->type->name() + "'");
    return update;
  }
  update->type = var->type;
  return update;
}

Expr *Sema::checkCall(CallExpr *call) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };

  if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
    auto *object = dyn_cast<NameExpr>(member->object);
    if (object && object->name == "console") {
      ScopeEntry shadow = lookup("console");
      if (!shadow.var && !shadow.func) {
        if (member->name == "log")
          return checkConsoleLog(call);
        diags_.error(member->loc(), "'console." + member->name +
                                        "' is not supported yet; only 'console.log' is");
        checkArgsLoosely();
        return call;
      }
    }
    diags_.error(member->loc(), "method calls are not supported yet");
    checkArgsLoosely();
    return call;
  }

  auto *name = dyn_cast<NameExpr>(call->callee);
  if (!name) {
    diags_.error(call->callee->loc(), "only functions can be called, by name, for now");
    checkArgsLoosely();
    return call;
  }

  ScopeEntry entry = lookup(name->name);
  if (!entry.func) {
    if (entry.var)
      diags_.error(name->loc(), "'" + name->name + "' is not a function");
    else
      diags_.error(name->loc(), "cannot find name '" + name->name + "'");
    checkArgsLoosely();
    return call;
  }

  FuncSymbol *func = entry.func;
  call->func = func;
  call->type = func->ret;
  if (call->args.size() != func->params.size()) {
    diags_.error(name->loc(), "expected " + llvm::Twine(func->params.size()) + " argument" +
                                  (func->params.size() == 1 ? "" : "s") + ", but got " +
                                  llvm::Twine(call->args.size()));
    checkArgsLoosely();
    return call;
  }
  for (size_t i = 0; i < call->args.size(); ++i)
    call->args[i] = coerce(checkExpr(call->args[i], func->params[i]), func->params[i]);
  return call;
}

Expr *Sema::checkConsoleLog(CallExpr *call) {
  call->builtin = Builtin::ConsoleLog;
  call->type = Type::voidTy();
  for (Expr *&arg : call->args) {
    arg = checkExpr(arg);
    const Type *type = arg->type;
    if (type->isVoid())
      diags_.error(arg->loc(), "an expression of type 'void' cannot be printed");
    else if (type->isString() && !isa<StringLitExpr>(arg))
      diags_.error(arg->loc(), "only string literals can be printed for now");
  }
  return call;
}

Expr *Sema::checkConditional(ConditionalExpr *cond, const Type *expected) {
  cond->cond = checkCondition(cond->cond);
  cond->thenExpr = checkExpr(cond->thenExpr, expected);
  cond->elseExpr = checkExpr(cond->elseExpr, expected);
  const Type *a = cond->thenExpr->type, *b = cond->elseExpr->type;
  cond->type = Type::error();
  if (a->isError() || b->isError())
    return cond;
  if (a == b) {
    cond->type = a;
    return cond;
  }
  if (a->isNumeric() && b->isNumeric()) {
    if (const Type *common = commonNumericType(a, b)) {
      cond->thenExpr = convert(cond->thenExpr, common);
      cond->elseExpr = convert(cond->elseExpr, common);
      cond->type = common;
      return cond;
    }
  }
  diags_.error(cond->loc(), "the branches of '?:' have incompatible types '" + a->name() + "' and '" +
                                b->name() + "'");
  return cond;
}

Expr *Sema::checkCast(CastExpr *cast) {
  cast->operand = checkExpr(cast->operand);
  const Type *from = cast->operand->type;
  const Type *to = resolveType(*cast->target, /*allowVoid=*/false);
  cast->type = to;
  if (from->isError() || to->isError())
    return cast;
  if (from == to || (from->isNumeric() && to->isNumeric()))
    return cast;
  diags_.error(cast->loc(), "cannot convert type '" + from->name() + "' to '" + to->name() + "'");
  return cast;
}

} // namespace tz
