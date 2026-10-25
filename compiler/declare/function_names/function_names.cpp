#include "compiler/declare/function_names/function_names.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction) {
  if (!stmt) {
    return;
  }
  auto expr = [&](const Expr *e) { collectNames(e, names, inFunction); };
  auto sub = [&](const Stmt *s) { collectNames(s, names, inFunction); };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      sub(s);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      if (d.init) {
        expr(d.init);
      }
    }
    break;
  case Stmt::Kind::Function: {
    auto *fn = cast<FunctionDecl>(stmt);
    for (const Param &param : fn->params) {
      if (param.defaultValue) {
        collectNames(param.defaultValue, names, true);
      }
    }
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
    if (s->cond) {
      expr(s->cond);
    }
    if (s->step) {
      expr(s->step);
    }
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
      if (c.test) {
        expr(c.test);
      }
      for (const Stmt *body : c.body) {
        sub(body);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    sub(cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    if (const Expr *value = cast<ReturnStmt>(stmt)->value) {
      expr(value);
    }
    break;
  case Stmt::Kind::Throw:
    expr(cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    sub(s->block);
    sub(s->handler);
    sub(s->finalizer);
    break;
  }
  case Stmt::Kind::Class: {
    // The methods and the initializers of the fields run as functions; the
    // static initializers where the class is declared.
    auto *cls = cast<ClassDecl>(stmt);
    if (cls->baseExpr) {
      collectNames(cls->baseExpr, names, inFunction);
    }
    for (const FunctionDecl *fn : cls->methods) {
      collectNames(fn, names, true);
    }
    for (const FunctionDecl *fn : cls->constructors) {
      collectNames(fn, names, true);
    }
    for (const ClassField &f : cls->fields) {
      if (f.init) {
        collectNames(f.init, names, f.isStatic ? inFunction : true);
      }
    }
    break;
  }
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Debugger:
    break;
  }
}

void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction) {
  auto sub = [&](const Expr *e) { collectNames(e, names, inFunction); };
  switch (expr->kind()) {
  case Expr::Kind::Name:
    if (inFunction) {
      names.insert(cast<NameExpr>(expr)->name);
    }
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      sub(e);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
      sub(e.expr);
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      if (p.key) {
        sub(p.key);
      }
      sub(p.value);
    }
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
    for (const Expr *arg : cast<CallExpr>(expr)->args) {
      sub(arg);
    }
    break;
  case Expr::Kind::New: {
    // Once checked, the arguments of a class's constructor are in its call.
    // `new C()` may name a variable that holds a class.
    auto *create = cast<NewExpr>(expr);
    if (inFunction && !create->name.empty()) {
      names.insert(create->name);
    }
    if (create->callee) {
      sub(create->callee);
    }
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args) {
      sub(arg);
    }
    break;
  }
  case Expr::Kind::Conditional:
    sub(cast<ConditionalExpr>(expr)->cond);
    sub(cast<ConditionalExpr>(expr)->thenExpr);
    sub(cast<ConditionalExpr>(expr)->elseExpr);
    break;
  case Expr::Kind::Cast:
    sub(cast<CastExpr>(expr)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
      sub(e);
    }
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain);
    break;
  case Expr::Kind::Function:
    collectNames(cast<FunctionExpr>(expr)->fn, names, true);
    break;
  case Expr::Kind::Yield:
    if (const Expr *value = cast<YieldExpr>(expr)->value) {
      sub(value);
    }
    break;
  case Expr::Kind::Await:
    sub(cast<AwaitExpr>(expr)->value);
    break;
  case Expr::Kind::Class: // as a declaration of the class
    collectNames(cast<ClassExpr>(expr)->decl, names, inFunction);
    break;
  default:
    break;
  }
}

} // namespace tz
