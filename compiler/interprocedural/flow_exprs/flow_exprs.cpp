#include "compiler/interprocedural/flow_exprs/flow_exprs.h"
#include "compiler/interprocedural/flow_calls/flow_calls.h"
#include "compiler/interprocedural/flow_stmts/flow_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FlowExprs::FlowExprs(FlowGraphBuilder &owner)
    : builder(owner) {}

const Expr *FlowExprs::ownExpr(const Expr *e) {
  while (auto *conv = llvm::dyn_cast<CastExpr>(e)) {
    e = conv->operand;
  }

  return e;
}

Refs FlowExprs::expr(Expr *e) {
  Refs refs = this->exprRefs(e);
  if (e && e->type && !FlowGraphBuilder::holdsReferences(e->type)) {
    return {};
  }

  return refs;
}

Refs FlowExprs::exprRefs(Expr *e) {
  if (!e) {
    return {};
  }
  switch (e->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    return {};
  case Expr::Kind::Template:
    for (Expr *part : llvm::cast<TemplateExpr>(e)->exprs) {
      this->expr(part);
    }
    return {};
  case Expr::Kind::ArrayLit: {
    Refs out;
    for (ArrayLitExpr::Element &el : llvm::cast<ArrayLitExpr>(e)->elements) {
      Refs value = this->expr(el.expr);
      FlowGraphBuilder::append(out, el.spread ? FlowGraphBuilder::copied(value)
                                              : FlowGraphBuilder::holding(value));
    }
    return out;
  }
  case Expr::Kind::ObjectLit: {
    auto *lit = llvm::cast<ObjectLitExpr>(e);
    Refs out;
    for (ObjectLitExpr::Property &p : lit->props) {
      if (p.key()) {
        this->expr(p.key());
      }
      Refs value = this->expr(p.value);
      if (p.keyed()) {
        this->builder.addFlow(value, this->builder.getUnknownNode());
      } else if (!p.spread() && !p.name.empty()) {
        this->builder.addFlow(value, this->builder.fieldNode(p.name));
      }
      FlowGraphBuilder::append(out, p.spread() ? FlowGraphBuilder::copied(value)
                                               : FlowGraphBuilder::holding(value));
    }
    if (lit->cls) {
      for (FunctionDecl *fn : lit->cls->methods) {
        this->builder.getFlowStmts().function(fn);
        if (fn->thisVar()) {
          this->builder.addFlow(out, this->builder.varNode(fn->thisVar()));
        }
      }
    }
    return out;
  }
  case Expr::Kind::Name: {
    auto *n = llvm::cast<NameExpr>(e);
    if (n->func()) {
      this->builder.rememberFunctionValue(n->func()->decl);
    }
    return this->builder.getFlowStmts().name(n);
  }
  case Expr::Kind::Member: {
    auto *member = llvm::cast<MemberExpr>(e);
    Refs object = this->expr(member->object);
    if (member->method()) {
      this->builder.getFlowCalls().passThis(object, member->method()->decl);
      this->builder.rememberFunctionValue(member->method()->decl);
      return {};
    }
    Refs out = FlowGraphBuilder::shifted(object, 1);
    out.push_back({this->builder.fieldNode(member->name), 0, 0});
    return out;
  }
  case Expr::Kind::Index: {
    auto *index = llvm::cast<IndexExpr>(e);
    Refs object = this->expr(index->object);
    this->expr(index->index);
    Refs out = FlowGraphBuilder::shifted(object, 1);
    const Type *objectType = index->object->type;
    if (int held = this->builder.contentsNode(objectType); held >= 0) {
      out.push_back({held, 0, 0});
    } else if (!index->field().empty()) {
      out.push_back({this->builder.fieldNode(index->field()), 0, 0});
    } else if (objectType && objectType->nonOptional()->isTuple()) {
      if (auto *key = llvm::dyn_cast<IntLitExpr>(index->index)) {
        out.push_back({this->builder.fieldNode(std::to_string(key->value)), 0, 0});
      }
    }
    return out;
  }
  case Expr::Kind::Unary: {
    auto *u = llvm::cast<UnaryExpr>(e);
    this->expr(u->operand);
    return Refs();
  }
  case Expr::Kind::Binary: {
    auto *b = llvm::cast<BinaryExpr>(e);
    Refs l = this->expr(b->lhs), r = this->expr(b->rhs);
    if (b->op != BinaryOp::LogAnd && b->op != BinaryOp::LogOr && b->op != BinaryOp::Nullish) {
      return {};
    }
    FlowGraphBuilder::append(l, r);
    return l;
  }
  case Expr::Kind::Assign: {
    auto *a = llvm::cast<AssignExpr>(e);
    Refs value = this->expr(a->value);
    this->builder.getFlowStmts().assignTo(a->target, value, a->value, a->op.has_value());
    return value;
  }
  case Expr::Kind::Update:
    this->builder.getFlowStmts().assignTo(llvm::cast<UpdateExpr>(e)->target, {}, nullptr, true);
    return {};
  case Expr::Kind::Call:
    return this->builder.getFlowCalls().call(llvm::cast<CallExpr>(e));
  case Expr::Kind::New: {
    auto *n = llvm::cast<NewExpr>(e);
    CallExpr *ctor = n->ctor();
    Refs out;
    std::vector<Refs> args;
    for (Expr *arg : ctor ? ctor->args : n->args) {
      args.push_back(this->expr(arg));
      FlowGraphBuilder::append(out, FlowGraphBuilder::holding(args.back()));
    }
    const FunctionDecl *target = !ctor                                    ? nullptr
                                 : ctor->method() && ctor->method()->decl ? ctor->method()->decl
                                 : ctor->func()                           ? ctor->func()->decl
                                                                          : nullptr;
    if (!target) {
      return out;
    }
    this->builder.getFlowCalls().passArguments(ctor, target, args);
    if (target->thisVar()) {
      out.push_back({this->builder.varNode(target->thisVar()), 0, 0});
      if (n->cls() && target->cls() && target->cls() != n->cls()) {
        this->builder.getFlowCalls().passThis(
            this->builder.getFlowCalls().receiverConversion(
                n, n->cls()->type, target, {}, target->thisVar(),
                "as 'this' of the constructor of '" + target->cls()->name.str() + "'"),
            target);
      }
    }
    return out;
  }
  case Expr::Kind::Conditional: {
    auto *c = llvm::cast<ConditionalExpr>(e);
    this->expr(c->cond);
    Refs out = this->expr(c->thenExpr);
    FlowGraphBuilder::append(out, this->expr(c->elseExpr));
    return out;
  }
  case Expr::Kind::Cast: {
    auto *conv = llvm::cast<CastExpr>(e);
    Refs out = this->expr(conv->operand);
    if (this->builder.isWideningCast(conv)) {
      int at = this->builder.findNode(NodeKind::Conversion, conv, "");
      auto *n = llvm::dyn_cast<NameExpr>(FlowExprs::ownExpr(conv->operand));
      this->builder.addConversion({at, conv->operand->type, conv->type, conv->loc(),
                                   this->builder.findNarrowings(conv->operand->type, conv->type),
                                   -1, "", n ? n->name : llvm::StringRef()});
      out.push_back({at, 0, 0});
    }
    return out;
  }
  case Expr::Kind::Sequence: {
    Refs last;
    for (Expr *part : llvm::cast<SequenceExpr>(e)->exprs) {
      last = this->expr(part);
    }
    return last;
  }
  case Expr::Kind::OptionalChain:
    return this->expr(llvm::cast<OptionalChainExpr>(e)->chain);
  case Expr::Kind::Function:
    this->builder.getFlowStmts().function(llvm::cast<FunctionExpr>(e)->fn);
    this->builder.rememberFunctionValue(llvm::cast<FunctionExpr>(e)->fn);
    return {};
  case Expr::Kind::Yield: {
    auto *y = llvm::cast<YieldExpr>(e);
    Refs value = this->expr(y->value);
    if (!this->builder.isInsideFunction()) {
      this->builder.escape(value);
    } else {
      this->builder.addFlow(y->delegate ? value : FlowGraphBuilder::holding(value),
                            this->builder.returnNode(this->builder.getCurrentFunction()));
    }
    return {};
  }
  case Expr::Kind::Await: {
    Expr *value = llvm::cast<AwaitExpr>(e)->value;
    Refs out = this->expr(value);
    if (int held = this->builder.contentsNode(value->type); held >= 0) {
      out.push_back({held, 0, 0});
    }
    return out;
  }
  case Expr::Kind::Class:
    break;
  }

  return {};
}

}
