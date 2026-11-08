#include "compiler/memory/store_exprs/store_exprs.h"
#include "compiler/memory/store_calls/store_calls.h"
#include "compiler/memory/store_stmts/store_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StoreExprs::StoreExprs(StoreScanner &scanner)
    : scanner(scanner) {}

StoreScanner::Sources StoreExprs::expr(const Expr *e) {
  if (!e) {
    return {};
  }
  StoreScanner::Sources out;
  if (this->scanner.isSite_(e)) {
    out.push_back(this->scanner.node({StoreScanner::Kind::Site, e}));
  }
  switch (e->kind()) {
  case Expr::Kind::Name: {
    auto *name = llvm::cast<NameExpr>(e);
    if (name->var()) {
      out.push_back(this->scanner.var(name->var()));
      if (name->var()->function() && !this->scanner.callee.contains(e)) {
        this->scanner.escapes(name->var()->function());
      }
      if (name->var()->lambda() && !this->scanner.callee.contains(e)) {
        this->scanner.escapes(name->var()->lambda());
      }
    }
    if (name->func() && !this->scanner.callee.contains(e)) {
      this->scanner.escapes(name->func()->decl);
    }
    break;
  }
  case Expr::Kind::Template:
    for (const Expr *x : llvm::cast<TemplateExpr>(e)->exprs) {
      expr(x);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &x : llvm::cast<ArrayLitExpr>(e)->elements) {
      StoreScanner::Sources element = expr(x.expr);
      if (!x.spread || (x.expr->type && x.expr->type->isIterator())) {
        this->scanner.store(element);
      }
    }
    break;
  case Expr::Kind::ObjectLit: {
    auto *lit = llvm::cast<ObjectLitExpr>(e);
    for (const ObjectLitExpr::Property &p : lit->props) {
      expr(p.key());
      if (p.method()) {
        if (auto *fe = llvm::dyn_cast<FunctionExpr>(p.value)) {
          this->scanner.function(fe->fn);
        }
        continue;
      }
      StoreScanner::Sources value = expr(p.value);
      bool binds = p.spread() && p.value->type && p.value->type->isObject() &&
                   llvm::any_of(p.value->type->fields(),
                                [](const Field &f) { return f.type->isFunction(); });
      if (!p.spread() || binds) {
        this->scanner.store(value);
      }
    }
    if (lit->cls) {
      for (const FunctionDecl *fn : lit->cls->methods) {
        this->scanner.function(fn);
        if (fn->thisVar()) {
          this->scanner.flowInto(this->scanner.var(fn->thisVar()), out);
        }
      }
      if (lit->cls->group) {
        for (const VarSymbol *v : lit->cls->group->captures) {
          this->scanner.edge(this->scanner.stored(), this->scanner.var(v));
        }
      }
    }
    break;
  }
  case Expr::Kind::Member: {
    auto *member = llvm::cast<MemberExpr>(e);
    StoreScanner::Sources object = expr(member->object);
    if (member->method() && !this->scanner.callee.contains(e)) {
      this->scanner.store(object);
      this->scanner.escapes(member->method()->decl);
    }
    if (member->type && member->type->nonOptional()->isFunction() &&
        !this->scanner.callee.contains(e)) {
      this->scanner.store(object);
    }
    out.clear();
    if (this->scanner.isSite_(e)) {
      out.push_back(this->scanner.node({StoreScanner::Kind::Site, e}));
    }
    break;
  }
  case Expr::Kind::Index:
    expr(llvm::cast<IndexExpr>(e)->object);
    expr(llvm::cast<IndexExpr>(e)->index);
    break;
  case Expr::Kind::Unary: {
    auto *unary = llvm::cast<UnaryExpr>(e);
    expr(unary->operand);
    break;
  }
  case Expr::Kind::Binary: {
    auto *bin = llvm::cast<BinaryExpr>(e);
    StoreScanner::Sources lhs = expr(bin->lhs);
    StoreScanner::Sources rhs =
        bin->op == BinaryOp::InstanceOf ? StoreScanner::Sources{} : expr(bin->rhs);
    if (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr || bin->op == BinaryOp::Nullish) {
      out.insert(out.end(), lhs.begin(), lhs.end());
      out.insert(out.end(), rhs.begin(), rhs.end());
    }
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = llvm::cast<AssignExpr>(e);
    StoreScanner::Sources value = expr(assign->value);
    if (auto *name = llvm::dyn_cast<NameExpr>(assign->target); name && name->var()) {
      this->scanner.flowInto(this->scanner.var(name->var()), value);
      out.push_back(this->scanner.var(name->var()));
    } else {
      if (auto *member = llvm::dyn_cast<MemberExpr>(assign->target)) {
        expr(member->object);
      } else if (auto *index = llvm::dyn_cast<IndexExpr>(assign->target)) {
        expr(index->object);
        expr(index->index);
      }
      this->scanner.store(value);
      out.insert(out.end(), value.begin(), value.end());
    }
    break;
  }
  case Expr::Kind::Update:
    expr(llvm::cast<UpdateExpr>(e)->target);
    break;
  case Expr::Kind::Call: {
    StoreScanner::Sources result = this->scanner.storeCalls().call(llvm::cast<CallExpr>(e));
    out.insert(out.end(), result.begin(), result.end());
    break;
  }
  case Expr::Kind::New: {
    auto *create = llvm::cast<NewExpr>(e);
    std::vector<StoreScanner::Sources> args;
    for (const Expr *arg : create->ctor() ? create->ctor()->args : create->args) {
      args.push_back(expr(arg));
    }
    if (const ClassDecl *cls = create->cls()) {
      const FunctionDecl *ctor =
          create->ctor() && create->ctor()->func() ? create->ctor()->func()->decl : nullptr;
      this->scanner.storeStmts().bindArgs(ctor, args);
      for (const ClassDecl *c = cls; c; c = c->parent) {
        if (c->init && c->init->thisVar()) {
          this->scanner.flowInto(this->scanner.var(c->init->thisVar()), out);
        }
        for (const FunctionDecl *k : c->constructors) {
          if (k->thisVar()) {
            this->scanner.flowInto(this->scanner.var(k->thisVar()), out);
          }
        }
      }
    } else {
      for (const StoreScanner::Sources &a : args) {
        this->scanner.store(a);
      }
    }
    break;
  }
  case Expr::Kind::Conditional: {
    auto *cond = llvm::cast<ConditionalExpr>(e);
    expr(cond->cond);
    StoreScanner::Sources a = expr(cond->thenExpr), b = expr(cond->elseExpr);
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    break;
  }
  case Expr::Kind::Cast: {
    auto *conv = llvm::cast<CastExpr>(e);
    StoreScanner::Sources operand = expr(conv->operand);
    if (conv->type &&
        (conv->type->nonOptional()->isUnknown() || conv->type->nonOptional()->isAnyObject())) {
      this->scanner.store(operand);
    } else {
      out.insert(out.end(), operand.begin(), operand.end());
    }
    break;
  }
  case Expr::Kind::Sequence: {
    StoreScanner::Sources last;
    for (const Expr *x : llvm::cast<SequenceExpr>(e)->exprs) {
      last = expr(x);
    }
    out.insert(out.end(), last.begin(), last.end());
    break;
  }
  case Expr::Kind::OptionalChain: {
    StoreScanner::Sources chain = expr(llvm::cast<OptionalChainExpr>(e)->chain);
    out.insert(out.end(), chain.begin(), chain.end());
    break;
  }
  case Expr::Kind::Function: {
    const FunctionDecl *fn = llvm::cast<FunctionExpr>(e)->fn;
    if (!fn->nonEscaping) {
      const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
      for (const VarSymbol *v : captures) {
        this->scanner.edge(this->scanner.stored(), this->scanner.var(v));
      }
      if (!this->scanner.callee.contains(e)) {
        this->scanner.escapes(fn);
      }
    }
    this->scanner.function(fn);
    break;
  }
  case Expr::Kind::Yield:
    this->scanner.store(expr(llvm::cast<YieldExpr>(e)->value));
    break;
  case Expr::Kind::Await: {
    StoreScanner::Sources value = expr(llvm::cast<AwaitExpr>(e)->value);
    this->scanner.store(value);
    break;
  }
  default:
    break;
  }

  return out;
}

}
