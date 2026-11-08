#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

ExprWalk::ExprWalk(Analysis &analysis)
    : analysis(analysis) {}

void ExprWalk::expr(const Expr *e, const Use &use) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    return;
  case Expr::Kind::Template:
    for (const Expr *x : cast<TemplateExpr>(e)->exprs) {
      expr(x);
    }
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &el : cast<ArrayLitExpr>(e)->elements) {
      if (el.hole) {
        continue;
      }
      Use inner;
      if (!el.spread) {
        inner.join = use.join;
      }
      expr(el.expr, inner);
      if (el.spread) {
        readSpread(el.expr, false, el.expr->loc());
      }
    }
    return;
  case Expr::Kind::ObjectLit: {
    auto *o = cast<ObjectLitExpr>(e);
    if (o->cls) {
      this->analysis.codeWalk.noteClass(o->cls);
    }
    for (const ObjectLitExpr::Property &p : o->props) {
      if (p.key()) {
        expr(p.key());
      }
      if (p.method()) {
        continue;
      }
      expr(p.value);
      if (p.spread()) {
        readSpread(p.value, true, p.loc);
      }
    }
    return;
  }
  case Expr::Kind::Name:
    name(cast<NameExpr>(e), use);
    return;
  case Expr::Kind::Member:
    member(cast<MemberExpr>(e));
    return;
  case Expr::Kind::Index: {
    auto *ix = cast<IndexExpr>(e);
    expr(ix->object);
    expr(ix->index);
    indexAccess(ix, false);
    return;
  }
  case Expr::Kind::Unary: {
    auto *u = cast<UnaryExpr>(e);
    Use inner;
    if (u->op == UnaryOp::Void) {
      inner.voided = true;
    }
    expr(u->operand, inner);
    return;
  }
  case Expr::Kind::Binary: {
    auto *b = cast<BinaryExpr>(e);
    if (b->op == BinaryOp::LogAnd || b->op == BinaryOp::LogOr || b->op == BinaryOp::Nullish) {
      expr(b->lhs, use);
      Active before = this->analysis.cur;
      expr(b->rhs, use);
      ParallelModel::merge(this->analysis.cur, before);
      return;
    }
    expr(b->lhs);
    expr(b->rhs);
    return;
  }
  case Expr::Kind::Assign:
    assign(cast<AssignExpr>(e));
    return;
  case Expr::Kind::Update:
    update(cast<UpdateExpr>(e)->target);
    return;
  case Expr::Kind::Call:
    this->analysis.callWalk.call(cast<CallExpr>(e), use);
    return;
  case Expr::Kind::New:
    this->analysis.callWalk.newExpr(cast<NewExpr>(e));
    return;
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(e);
    expr(c->cond);
    Active before = this->analysis.cur;
    expr(c->thenExpr, use);
    Active then = std::move(this->analysis.cur);
    this->analysis.cur = std::move(before);
    expr(c->elseExpr, use);
    ParallelModel::merge(this->analysis.cur, then);
    return;
  }
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(e);
    if (c->type && c->type->isFunction() &&
        this->analysis.callWalk.functionRef(c->operand, use, c->type)) {
      expr(c->operand, use);
      return;
    }
    if (c->type && c->type->isUnknown() && c->operand->type &&
        c->operand->type->nonOptional()->isFunction()) {
      this->analysis.callWalk.unknownRef(c->operand);
    }
    expr(c->operand, c->isImplicit() ? use : Use());
    if (c->type && c->type->isString() && ParallelHelpers::holdsReferences(c->operand->type)) {
      contents(c->operand, false, c->loc());
    }
    return;
  }
  case Expr::Kind::Sequence: {
    auto *s = cast<SequenceExpr>(e);
    for (size_t i = 0; i < s->exprs.size(); ++i) {
      expr(s->exprs[i], i + 1 == s->exprs.size() ? use : Use());
    }
    return;
  }
  case Expr::Kind::OptionalChain: {
    Active before = this->analysis.cur;
    expr(cast<OptionalChainExpr>(e)->chain, use);
    ParallelModel::merge(this->analysis.cur, before);
    return;
  }
  case Expr::Kind::Function: {
    auto *fe = cast<FunctionExpr>(e);
    this->analysis.codeWalk.discover(fe->fn);
    this->analysis.callWalk.functionRef(fe, use, fe->type);
    return;
  }
  case Expr::Kind::Yield:
    yieldExpr(cast<YieldExpr>(e));
    return;
  case Expr::Kind::Await:
    awaitExpr(cast<AwaitExpr>(e));
    return;
  case Expr::Kind::Class:
    break;
  }
}

void ExprWalk::name(const NameExpr *n, const Use &use) {
  if (n->func() || (n->var() && (n->var()->function() || n->var()->lambda()))) {
    this->analysis.callWalk.functionRef(n, use, n->type);
    return;
  }
  this->analysis.codeWalk.var(n->var(), false, n->loc());
}

void ExprWalk::member(const MemberExpr *m) {
  expr(m->object);
  if (m->method()) {
    this->analysis.callWalk.functionRef(m, {}, m->type);
    return;
  }
  fieldAccess(m->object, m->name, false, m->loc());
}

void ExprWalk::fieldAccess(const Expr *object, llvm::StringRef name, bool write, llvm::SMLoc at) {
  const Type *type = object && object->type ? object->type->nonOptional() : nullptr;
  if (!type) {
    return;
  }
  if (type->isArray()) {
    if (name == "length") {
      this->analysis.codeWalk.touch(LocKind::Field, this->analysis.aliasAnalysis.refOf(object),
                                    this->analysis.length, write, at);
      if (write) {
        this->analysis.codeWalk.touch(LocKind::Elements, this->analysis.aliasAnalysis.refOf(object),
                                      nullptr, true, at);
      }
    }
    return;
  }
  if (type->isTypedArray()) {
    return;
  }
  if (type->isJson()) {
    this->analysis.codeWalk.touch(LocKind::Elements, this->analysis.aliasAnalysis.refOf(object),
                                  nullptr, write, at);
    return;
  }
  const Field *field = nullptr;
  if (type->isObject()) {
    field = type->field(name);
  } else if (type->isUnion()) {
    for (const Type *m : type->members()) {
      if (m->isObject() && (field = m->field(name))) {
        break;
      }
    }
  }
  if (!field) {
    return;
  }
  for (const ClassDecl *k = type->classDecl(); k; k = k->parent) {
    if (k->library() &&
        llvm::any_of(k->fields, [&](const ClassField &f) { return f.name == name; })) {
      this->analysis.libraryEffects.libraryState(ParallelHelpers::libraryClassName(k), name,
                                                 this->analysis.aliasAnalysis.receiverRef(object),
                                                 write, at);
      return;
    }
  }
  if (!write && (field->readonly || (field->type && field->type->isLiteral()))) {
    return;
  }
  this->analysis.codeWalk.touch(LocKind::Field, this->analysis.aliasAnalysis.refOf(object),
                                this->analysis.intern(name), write, at);
}

void ExprWalk::indexAccess(const IndexExpr *ix, bool write) {
  if (ix->enumDecl() || !ix->object->type) {
    return;
  }
  if (!ix->field().empty()) {
    fieldAccess(ix->object, ix->field(), write, ix->loc());
    return;
  }
  const Type *type = ix->object->type->nonOptional();
  if (ix->keyed()) {
    this->analysis.codeWalk.touch(LocKind::Field, this->analysis.aliasAnalysis.refOf(ix->object),
                                  this->analysis.star, write, ix->loc());
    return;
  }
  if (type->isString()) {
    return;
  }
  if (type->isArray()) {
    auto *index = dyn_cast<NameExpr>(ParallelHelpers::strip(ix->index));
    if (index && index->var() && this->analysis.ownIndex.contains(index->var()) &&
        !index->var()->reassigned && !ix->mayAppend()) {
      this->analysis.codeWalk.touch(LocKind::OwnCell,
                                    this->analysis.aliasAnalysis.refOf(ix->object),
                                    index->var()->owner(), write, ix->loc());
      return;
    }
    if (write && ix->mayAppend()) {
      this->analysis.codeWalk.touch(LocKind::Field, this->analysis.aliasAnalysis.refOf(ix->object),
                                    this->analysis.length, true, ix->loc());
    }
  }
  this->analysis.codeWalk.touch(LocKind::Elements, this->analysis.aliasAnalysis.refOf(ix->object),
                                nullptr, write, ix->loc());
}

void ExprWalk::elements(const Expr *e, bool write, llvm::SMLoc at) {
  if (e && ParallelHelpers::holdsReferences(e->type)) {
    this->analysis.codeWalk.touch(LocKind::Elements, this->analysis.aliasAnalysis.refOf(e), nullptr,
                                  write, at);
  }
}

void ExprWalk::contents(const Expr *e, bool write, llvm::SMLoc at) {
  if (!e || !ParallelHelpers::holdsReferences(e->type)) {
    return;
  }
  const Expr *made = ParallelHelpers::strip(e);
  if (auto *lit = dyn_cast<ArrayLitExpr>(made)) {
    for (const ArrayLitExpr::Element &el : lit->elements) {
      if (!el.hole) {
        contents(el.expr, write, at);
      }
    }
    return;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(made)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      if (!p.method()) {
        contents(p.value, write, at);
      }
    }
    return;
  }
  Ref ref = this->analysis.aliasAnalysis.refOf(e);
  if (ref.kind != RefKind::Local) {
    this->analysis.codeWalk.touch(LocKind::Contents, ref, nullptr, write, at);
    return;
  }
  const Type *type = e->type->nonOptional();
  if (llvm::any_of(this->analysis.aliasAnalysis.inside(type), ParallelHelpers::holdsReferences)) {
    this->analysis.codeWalk.touch(LocKind::Contents, Ref{RefKind::Type, type}, this->analysis.inner,
                                  write, at);
  }
}

void ExprWalk::readSpread(const Expr *e, bool object, llvm::SMLoc at) {
  const Type *type = e && e->type ? e->type->nonOptional() : nullptr;
  if (!type) {
    return;
  }
  if (type->isIterator() || type->isAsyncIterator()) {
    this->analysis.codeWalk.iterate(e, e);
    return;
  }
  if (object && type->isObject()) {
    this->analysis.codeWalk.touch(LocKind::Field, this->analysis.aliasAnalysis.refOf(e),
                                  this->analysis.star, false, at);
    return;
  }
  elements(e, false, at);
}

void ExprWalk::assign(const AssignExpr *a) {
  bool compound = a->op.has_value();
  bool logical = compound && (*a->op == BinaryOp::LogAnd || *a->op == BinaryOp::LogOr ||
                              *a->op == BinaryOp::Nullish);
  auto value = [&] {
    if (!logical) {
      expr(a->value);
      return;
    }
    Active before = this->analysis.cur;
    expr(a->value);
    ParallelModel::merge(this->analysis.cur, before);
  };
  const Expr *target = a->target;
  if (auto *n = dyn_cast<NameExpr>(target)) {
    if (compound) {
      this->analysis.codeWalk.var(n->var(), false, n->loc());
    }
    value();
    this->analysis.codeWalk.var(n->var(), true, n->loc());
    return;
  }
  if (auto *m = dyn_cast<MemberExpr>(target)) {
    expr(m->object);
    if (compound) {
      fieldAccess(m->object, m->name, false, m->loc());
    }
    value();
    fieldAccess(m->object, m->name, true, m->loc());
    return;
  }
  if (auto *ix = dyn_cast<IndexExpr>(target)) {
    expr(ix->object);
    expr(ix->index);
    if (compound) {
      indexAccess(ix, false);
    }
    value();
    indexAccess(ix, true);
    return;
  }
  expr(target);
  value();
}

void ExprWalk::update(const Expr *target) {
  if (auto *n = dyn_cast<NameExpr>(target)) {
    this->analysis.codeWalk.var(n->var(), false, n->loc());
    this->analysis.codeWalk.var(n->var(), true, n->loc());
  } else if (auto *m = dyn_cast<MemberExpr>(target)) {
    expr(m->object);
    fieldAccess(m->object, m->name, false, m->loc());
    fieldAccess(m->object, m->name, true, m->loc());
  } else if (auto *ix = dyn_cast<IndexExpr>(target)) {
    expr(ix->object);
    expr(ix->index);
    indexAccess(ix, false);
    indexAccess(ix, true);
  } else {
    expr(target);
  }
}

void ExprWalk::awaitExpr(const AwaitExpr *a) {
  auto *c = dyn_cast<CallExpr>(ParallelHelpers::strip(a->value));
  bool join = c && c->builtin() == Builtin::PromiseAll;
  if (join) {
    this->analysis.openJoins.push_back({a, this->analysis.cur});
  }
  Use use;
  use.awaited = a;
  expr(a->value, use);
  this->analysis.awaits[a] = {this->analysis.fi};
  if (join) {
    OpenJoin open = std::move(this->analysis.openJoins.back());
    this->analysis.openJoins.pop_back();
    ParallelModel::merge(open.active, this->analysis.cur);
    JoinInfo &info = this->analysis.joins[a];
    info.fn = this->analysis.fi;
    ParallelModel::merge(info.segs, open.active);
  }
  this->analysis.codeWalk.moveTo(this->analysis.codeWalk.segment(a, SegKind::Await, a->loc()));
}

void ExprWalk::yieldExpr(const YieldExpr *y) {
  expr(y->value);
  if (y->delegate && y->value) {
    this->analysis.codeWalk.iterate(y->value, y);
  }
  if (!this->analysis.fn->asyncGen) {
    return;
  }
  unsigned seg =
      this->analysis.codeWalk.segment(y, y->delegate ? SegKind::Await : SegKind::Yield, y->loc());
  this->analysis.fn->segs[seg].delegate = y->delegate;
  this->analysis.codeWalk.moveTo(seg);
}

}
