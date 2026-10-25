#include "compiler/memory/cycle_escapes/cycle_escapes.h"

namespace tz {

CycleEscapes::CycleEscapes(CycleScanner &scanner) : scanner_(scanner) {}

void CycleEscapes::ordered(const Stmt *stmt, const VarSymbol *self, bool &escaped, bool afterSuper,
                           llvm::DenseSet<const AssignExpr *> &safe) {
  if (!stmt) {
    return;
  }
  auto expr = [&](const Expr *e) { orderedExpr(e, self, escaped, afterSuper, safe); };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : llvm::cast<BlockStmt>(stmt)->body) {
      ordered(s, self, escaped, afterSuper, safe);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : llvm::cast<VarDeclStmt>(stmt)->decls) {
      expr(d.init);
    }
    break;
  case Stmt::Kind::Expr:
    expr(llvm::cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::Return:
    expr(llvm::cast<ReturnStmt>(stmt)->value);
    break;
  case Stmt::Kind::Throw:
    expr(llvm::cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::If: {
    auto *s = llvm::cast<IfStmt>(stmt);
    expr(s->cond);
    bool before = escaped;
    ordered(s->thenStmt, self, escaped, afterSuper, safe);
    bool then = escaped;
    escaped = before;
    ordered(s->elseStmt, self, escaped, afterSuper, safe);
    escaped = escaped || then;
    break;
  }
  case Stmt::Kind::Labeled:
    ordered(llvm::cast<LabeledStmt>(stmt)->body, self, escaped, afterSuper, safe);
    break;
  case Stmt::Kind::Function: {
    // A nested function that captures `self` lets it go.
    auto *fn = llvm::cast<FunctionDecl>(stmt);
    escaped = escaped || llvm::is_contained(fn->captures, self);
    break;
  }
  default:
    // A loop may run its body again after `self` went in it, and so may a
    // `switch` fall through or a `try` jump: their writes count as late if
    // `self` goes anywhere in them.
    escaped = escaped || CycleScanner::thisEscapes(stmt, self);
    walkStmt(stmt, [&](const Expr *e) {
      bool late = true;
      orderedExpr(e, self, late, afterSuper, safe);
    });
    break;
  }
}

void CycleEscapes::orderedExpr(const Expr *e, const VarSymbol *self, bool &escaped, bool afterSuper,
                               llvm::DenseSet<const AssignExpr *> &safe) {
  if (!e) {
    return;
  }
  if (auto *assign = llvm::dyn_cast<AssignExpr>(e)) {
    auto *member = llvm::dyn_cast<MemberExpr>(assign->target);
    auto *object = member ? llvm::dyn_cast<NameExpr>(member->object) : nullptr;
    if (object && object->var == self && !assign->op) {
      orderedExpr(assign->value, self, escaped, afterSuper, safe);
      if (!escaped && !mentions(assign->value, self)) {
        safe.insert(assign);
      }
      return;
    }
  }
  if (auto *call = llvm::dyn_cast<CallExpr>(e);
      call && call->superCall && !llvm::isa<MemberExpr>(call->callee)) {
    for (const Expr *arg : call->args) {
      escaped = escaped || exprEscapes(arg, self);
    }
    escaped = escaped || afterSuper;
    return;
  }
  escaped = escaped || exprEscapes(e, self);
}

bool CycleEscapes::exprEscapes(const Expr *expr, const VarSymbol *self) {
  ExprStmt stmt(llvm::SMLoc(), const_cast<Expr *>(expr));

  return CycleScanner::thisEscapes(&stmt, self);
}

bool CycleEscapes::constructionEscapes(const ClassDecl *cls) {
  auto [it, inserted] = scanner_.escapes_.try_emplace(cls, true);
  if (!inserted) {
    return it->second;
  }
  bool escapes = cls->parent && constructionEscapes(cls->parent);
  for (const FunctionDecl *ctor : cls->constructors) {
    escapes = escapes || (ctor->thisVar && CycleScanner::thisEscapes(ctor->body, ctor->thisVar));
  }
  if (cls->init && cls->init->thisVar) {
    escapes = escapes || CycleScanner::thisEscapes(cls->init->body, cls->init->thisVar);
  }
  scanner_.escapes_[cls] = escapes;

  return escapes;
}

bool CycleEscapes::mentions(const Expr *expr, const VarSymbol *self) {
  bool found = false;
  std::function<void(const Expr *)> visit = [&](const Expr *e) {
    if (!e || found) {
      return;
    }
    if (auto *name = llvm::dyn_cast<NameExpr>(e)) {
      found = name->var == self;
      return;
    }
    if (auto *fe = llvm::dyn_cast<FunctionExpr>(e)) {
      found = llvm::is_contained(fe->fn->captures, self) || mentionsIn(fe->fn->body, self);
      return;
    }
    forEachChild(e, visit);
  };
  visit(expr);

  return found;
}

bool CycleEscapes::mentionsIn(const Stmt *stmt, const VarSymbol *self) {
  bool found = false;
  walkStmt(stmt, [&](const Expr *e) { found = found || mentions(e, self); });

  return found;
}

void CycleEscapes::forEachChild(const Expr *e, const std::function<void(const Expr *)> &visit) {
  switch (e->kind()) {
  case Expr::Kind::Template:
    for (const Expr *x : llvm::cast<TemplateExpr>(e)->exprs) {
      visit(x);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &x : llvm::cast<ArrayLitExpr>(e)->elements) {
      visit(x.expr);
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : llvm::cast<ObjectLitExpr>(e)->props) {
      visit(p.key);
      visit(p.value);
    }
    break;
  case Expr::Kind::Member:
    visit(llvm::cast<MemberExpr>(e)->object);
    break;
  case Expr::Kind::Index:
    visit(llvm::cast<IndexExpr>(e)->object);
    visit(llvm::cast<IndexExpr>(e)->index);
    break;
  case Expr::Kind::Unary:
    visit(llvm::cast<UnaryExpr>(e)->operand);
    break;
  case Expr::Kind::Binary:
    visit(llvm::cast<BinaryExpr>(e)->lhs);
    visit(llvm::cast<BinaryExpr>(e)->rhs);
    break;
  case Expr::Kind::Assign:
    visit(llvm::cast<AssignExpr>(e)->target);
    visit(llvm::cast<AssignExpr>(e)->value);
    break;
  case Expr::Kind::Update:
    visit(llvm::cast<UpdateExpr>(e)->target);
    break;
  case Expr::Kind::Call:
    visit(llvm::cast<CallExpr>(e)->callee);
    for (const Expr *x : llvm::cast<CallExpr>(e)->args) {
      visit(x);
    }
    break;
  case Expr::Kind::New: {
    auto *create = llvm::cast<NewExpr>(e);
    for (const Expr *x : create->ctor ? create->ctor->args : create->args) {
      visit(x);
    }
    break;
  }
  case Expr::Kind::Conditional:
    visit(llvm::cast<ConditionalExpr>(e)->cond);
    visit(llvm::cast<ConditionalExpr>(e)->thenExpr);
    visit(llvm::cast<ConditionalExpr>(e)->elseExpr);
    break;
  case Expr::Kind::Cast:
    visit(llvm::cast<CastExpr>(e)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *x : llvm::cast<SequenceExpr>(e)->exprs) {
      visit(x);
    }
    break;
  case Expr::Kind::OptionalChain:
    visit(llvm::cast<OptionalChainExpr>(e)->chain);
    break;
  case Expr::Kind::Yield:
    visit(llvm::cast<YieldExpr>(e)->value);
    break;
  case Expr::Kind::Await:
    visit(llvm::cast<AwaitExpr>(e)->value);
    break;
  default:
    break;
  }
}

void CycleEscapes::walkStmt(const Stmt *stmt, const std::function<void(const Expr *)> &visit) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : llvm::cast<BlockStmt>(stmt)->body) {
      walkStmt(s, visit);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : llvm::cast<VarDeclStmt>(stmt)->decls) {
      visit(d.init);
    }
    break;
  case Stmt::Kind::Expr:
    visit(llvm::cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If:
    visit(llvm::cast<IfStmt>(stmt)->cond);
    walkStmt(llvm::cast<IfStmt>(stmt)->thenStmt, visit);
    walkStmt(llvm::cast<IfStmt>(stmt)->elseStmt, visit);
    break;
  case Stmt::Kind::While:
    visit(llvm::cast<WhileStmt>(stmt)->cond);
    walkStmt(llvm::cast<WhileStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::DoWhile:
    walkStmt(llvm::cast<DoWhileStmt>(stmt)->body, visit);
    visit(llvm::cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For:
    walkStmt(llvm::cast<ForStmt>(stmt)->init, visit);
    visit(llvm::cast<ForStmt>(stmt)->cond);
    visit(llvm::cast<ForStmt>(stmt)->step);
    walkStmt(llvm::cast<ForStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::ForOf:
    visit(llvm::cast<ForOfStmt>(stmt)->iterable);
    walkStmt(llvm::cast<ForOfStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::Switch:
    visit(llvm::cast<SwitchStmt>(stmt)->discriminant);
    for (const SwitchCase &c : llvm::cast<SwitchStmt>(stmt)->cases) {
      visit(c.test);
      for (const Stmt *s : c.body) {
        walkStmt(s, visit);
      }
    }
    break;
  case Stmt::Kind::Labeled:
    walkStmt(llvm::cast<LabeledStmt>(stmt)->body, visit);
    break;
  case Stmt::Kind::Return:
    visit(llvm::cast<ReturnStmt>(stmt)->value);
    break;
  case Stmt::Kind::Throw:
    visit(llvm::cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try:
    walkStmt(llvm::cast<TryStmt>(stmt)->block, visit);
    walkStmt(llvm::cast<TryStmt>(stmt)->handler, visit);
    walkStmt(llvm::cast<TryStmt>(stmt)->finalizer, visit);
    break;
  case Stmt::Kind::Function: {
    // A nested function that captures `this` lets it go.
    auto *fn = llvm::cast<FunctionDecl>(stmt);
    for (const VarSymbol *var : fn->captures) {
      NameExpr fake(llvm::SMLoc(), var->name);
      fake.var = const_cast<VarSymbol *>(var);
      visit(&fake);
    }
    break;
  }
  default:
    break;
  }
}

} // namespace tz
