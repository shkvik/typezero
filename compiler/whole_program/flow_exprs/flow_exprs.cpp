#include "compiler/whole_program/flow_exprs/flow_exprs.h"
#include "compiler/whole_program/flow_calls/flow_calls.h"
#include "compiler/whole_program/flow_stmts/flow_stmts.h"
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

FlowExprs::FlowExprs(Flows &flows) : flows_(flows) {}

const Expr *FlowExprs::ownExpr(const Expr *e) {
  while (auto *conv = llvm::dyn_cast<CastExpr>(e)) {
    e = conv->operand;
  }

  return e;
}

Refs FlowExprs::expr(Expr *e) {
  Refs refs = exprRefs(e);
  if (e && e->type && !holdsReferences(e->type)) {
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
      expr(part);
    }
    return {};
  case Expr::Kind::ArrayLit: {
    Refs out;
    for (ArrayLitExpr::Element &el : llvm::cast<ArrayLitExpr>(e)->elements) {
      Refs value = expr(el.expr);
      append(out, el.spread ? copied(value) : holding(value));
    }
    return out;
  }
  case Expr::Kind::ObjectLit: {
    auto *lit = llvm::cast<ObjectLitExpr>(e);
    Refs out;
    for (ObjectLitExpr::Property &p : lit->props) {
      if (p.key) {
        expr(p.key);
      }
      Refs value = expr(p.value);
      if (!p.spread && !p.name.empty()) {
        flows_.flow(value, flows_.fieldNode(p.name));
      }
      append(out, p.spread ? copied(value) : holding(value));
    }
    if (lit->cls) {
      for (FunctionDecl *fn : lit->cls->methods) {
        flows_.flowStmts().function(fn);
        if (fn->thisVar) {
          flows_.flow(out, flows_.var(fn->thisVar));
        }
      }
    }
    return out;
  }
  case Expr::Kind::Name: {
    auto *n = llvm::cast<NameExpr>(e);
    if (n->func) {
      flows_.flowCalls().functionValue(n->func->decl);
    }
    return flows_.flowStmts().name(n);
  }
  case Expr::Kind::Member: {
    auto *member = llvm::cast<MemberExpr>(e);
    Refs object = expr(member->object);
    if (member->method) {
      flows_.flowCalls().passThis(object, member->method->decl);
      flows_.flowCalls().functionValue(member->method->decl);
      return {};
    }
    Refs out = shifted(object, 1);
    out.push_back({flows_.fieldNode(member->name), 0, 0});
    return out;
  }
  case Expr::Kind::Index: {
    auto *index = llvm::cast<IndexExpr>(e);
    Refs object = expr(index->object);
    expr(index->index);
    Refs out = shifted(object, 1);
    const Type *objectType = index->object->type;
    if (int held = flows_.contents(objectType); held >= 0) {
      out.push_back({held, 0, 0});
    } else if (!index->field.empty()) {
      out.push_back({flows_.fieldNode(index->field), 0, 0});
    } else if (objectType && objectType->nonOptional()->isTuple()) {
      if (auto *key = llvm::dyn_cast<IntLitExpr>(index->index)) {
        out.push_back({flows_.fieldNode(std::to_string(key->value)), 0, 0});
      }
    }
    return out;
  }
  case Expr::Kind::Unary: {
    auto *u = llvm::cast<UnaryExpr>(e);
    expr(u->operand);
    return Refs();
  }
  case Expr::Kind::Binary: {
    auto *b = llvm::cast<BinaryExpr>(e);
    Refs l = expr(b->lhs), r = expr(b->rhs);
    if (b->op != BinaryOp::LogAnd && b->op != BinaryOp::LogOr && b->op != BinaryOp::Nullish) {
      return {};
    }
    append(l, r);
    return l;
  }
  case Expr::Kind::Assign: {
    auto *a = llvm::cast<AssignExpr>(e);
    Refs value = expr(a->value);
    flows_.flowStmts().assignTo(a->target, value, a->value, a->op.has_value());
    return value;
  }
  case Expr::Kind::Update:
    flows_.flowStmts().assignTo(llvm::cast<UpdateExpr>(e)->target, {}, nullptr, true);
    return {};
  case Expr::Kind::Call:
    return flows_.flowCalls().call(llvm::cast<CallExpr>(e));
  case Expr::Kind::New: {
    auto *n = llvm::cast<NewExpr>(e);
    CallExpr *ctor = n->ctor;
    Refs out;
    std::vector<Refs> args;
    for (Expr *arg : ctor ? ctor->args : n->args) {
      args.push_back(expr(arg));
      append(out, holding(args.back()));
    }
    const FunctionDecl *target = !ctor                                ? nullptr
                                 : ctor->method && ctor->method->decl ? ctor->method->decl
                                 : ctor->func                         ? ctor->func->decl
                                                                      : nullptr;
    if (!target) {
      return out;
    }
    flows_.flowCalls().passArguments(ctor, target, args);
    if (target->thisVar) {
      out.push_back({flows_.var(target->thisVar), 0, 0});
      if (n->cls && target->cls && target->cls != n->cls) {
        flows_.flowCalls().passThis(
            flows_.flowCalls().receiverConversion(n, n->cls->type, target, {}, target->thisVar,
                                                  "as 'this' of the constructor of '" +
                                                      target->cls->name.str() + "'"),
            target);
      }
    }
    return out;
  }
  case Expr::Kind::Conditional: {
    auto *c = llvm::cast<ConditionalExpr>(e);
    expr(c->cond);
    Refs out = expr(c->thenExpr);
    append(out, expr(c->elseExpr));
    return out;
  }
  case Expr::Kind::Cast: {
    auto *conv = llvm::cast<CastExpr>(e);
    Refs out = expr(conv->operand);
    if (flows_.widenings_.contains(conv)) {
      int at = flows_.node(NodeKind::Conversion, conv, "");
      auto *n = llvm::dyn_cast<NameExpr>(ownExpr(conv->operand));
      flows_.conversions.push_back({at, conv->operand->type, conv->type, conv->loc(),
                                    flows_.narrowings_(conv->operand->type, conv->type), -1, "",
                                    n ? n->name : llvm::StringRef()});
      out.push_back({at, 0, 0});
    }
    return out;
  }
  case Expr::Kind::Sequence: {
    Refs last;
    for (Expr *part : llvm::cast<SequenceExpr>(e)->exprs) {
      last = expr(part);
    }
    return last;
  }
  case Expr::Kind::OptionalChain:
    return expr(llvm::cast<OptionalChainExpr>(e)->chain);
  case Expr::Kind::Function:
    flows_.flowStmts().function(llvm::cast<FunctionExpr>(e)->fn);
    flows_.flowCalls().functionValue(llvm::cast<FunctionExpr>(e)->fn);
    return {};
  case Expr::Kind::Yield:
    flows_.escape(expr(llvm::cast<YieldExpr>(e)->value));
    return {};
  case Expr::Kind::Await: {
    Expr *value = llvm::cast<AwaitExpr>(e)->value;
    Refs out = expr(value);
    if (int held = flows_.contents(value->type); held >= 0) {
      out.push_back({held, 0, 0});
    }
    return out;
  }
  }

  return {};
}

}
