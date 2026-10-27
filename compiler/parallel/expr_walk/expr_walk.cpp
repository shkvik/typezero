#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/pieces/pieces.h"
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

ExprWalk::ExprWalk(Analysis &analysis) : analysis_(analysis) {}

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
      analysis_.codeWalk().noteClass(o->cls);
    }
    for (const ObjectLitExpr::Property &p : o->props) {
      if (p.key) {
        expr(p.key);
      }
      if (p.method) {
        continue;
      }
      expr(p.value);
      if (p.spread) {
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
      Active before = analysis_.cur_;
      expr(b->rhs, use);
      merge(analysis_.cur_, before);
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
    analysis_.callWalk().call(cast<CallExpr>(e), use);
    return;
  case Expr::Kind::New:
    analysis_.callWalk().newExpr(cast<NewExpr>(e));
    return;
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(e);
    expr(c->cond);
    Active before = analysis_.cur_;
    expr(c->thenExpr, use);
    Active then = std::move(analysis_.cur_);
    analysis_.cur_ = std::move(before);
    expr(c->elseExpr, use);
    merge(analysis_.cur_, then);
    return;
  }
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(e);
    if (c->type && c->type->isFunction() &&
        analysis_.callWalk().functionRef(c->operand, use, c->type)) {
      expr(c->operand, use);
      return;
    }
    expr(c->operand, c->isImplicit() ? use : Use());
    if (c->type && c->type->isString() && holdsReferences(c->operand->type)) {
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
    Active before = analysis_.cur_;
    expr(cast<OptionalChainExpr>(e)->chain, use);
    merge(analysis_.cur_, before);
    return;
  }
  case Expr::Kind::Function: {
    auto *fe = cast<FunctionExpr>(e);
    analysis_.codeWalk().discover(fe->fn);
    analysis_.callWalk().functionRef(fe, use, fe->type);
    return;
  }
  case Expr::Kind::Yield:
    yieldExpr(cast<YieldExpr>(e));
    return;
  case Expr::Kind::Await:
    awaitExpr(cast<AwaitExpr>(e));
    return;
  }
}

void ExprWalk::name(const NameExpr *n, const Use &use) {
  if (n->func || (n->var && (n->var->function || n->var->lambda))) {
    analysis_.callWalk().functionRef(n, use, n->type);
    return;
  }
  analysis_.codeWalk().var(n->var, false, n->loc());
}

void ExprWalk::member(const MemberExpr *m) {
  expr(m->object);
  if (m->method) {
    analysis_.callWalk().functionRef(m, {}, m->type);
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
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(object),
                                 analysis_.length_, write, at);
      if (write) {
        analysis_.codeWalk().touch(LocKind::Elements, analysis_.aliasAnalysis().refOf(object),
                                   nullptr, true, at);
      }
    }
    return;
  }
  if (type->isTypedArray()) {
    return;
  }
  if (type->isJson()) {
    analysis_.codeWalk().touch(LocKind::Elements, analysis_.aliasAnalysis().refOf(object), nullptr,
                               write, at);
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
  if (!write && (field->readonly || (field->type && field->type->isLiteral()))) {
    return;
  }
  analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(object),
                             analysis_.intern(name), write, at);
}

void ExprWalk::indexAccess(const IndexExpr *ix, bool write) {
  if (ix->enumDecl || !ix->object->type) {
    return;
  }
  if (!ix->field.empty()) {
    fieldAccess(ix->object, ix->field, write, ix->loc());
    return;
  }
  const Type *type = ix->object->type->nonOptional();
  if (ix->keyed) {
    analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(ix->object),
                               analysis_.star_, write, ix->loc());
    return;
  }
  if (type->isString()) {
    return;
  }
  if (type->isArray()) {
    auto *index = dyn_cast<NameExpr>(strip(ix->index));
    if (index && index->var && analysis_.ownIndex_.contains(index->var) &&
        !index->var->reassigned && !ix->mayAppend) {
      analysis_.codeWalk().touch(LocKind::OwnCell, analysis_.aliasAnalysis().refOf(ix->object),
                                 index->var->owner, write, ix->loc());
      return;
    }
    if (write && ix->mayAppend) {
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(ix->object),
                                 analysis_.length_, true, ix->loc());
    }
  }
  analysis_.codeWalk().touch(LocKind::Elements, analysis_.aliasAnalysis().refOf(ix->object),
                             nullptr, write, ix->loc());
}

void ExprWalk::elements(const Expr *e, bool write, llvm::SMLoc at) {
  if (e && holdsReferences(e->type)) {
    analysis_.codeWalk().touch(LocKind::Elements, analysis_.aliasAnalysis().refOf(e), nullptr,
                               write, at);
  }
}

void ExprWalk::contents(const Expr *e, bool write, llvm::SMLoc at) {
  if (!e || !holdsReferences(e->type)) {
    return;
  }
  const Expr *made = strip(e);
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
      if (!p.method) {
        contents(p.value, write, at);
      }
    }
    return;
  }
  Ref ref = analysis_.aliasAnalysis().refOf(e);
  if (ref.kind != RefKind::Local) {
    analysis_.codeWalk().touch(LocKind::Contents, ref, nullptr, write, at);
    return;
  }
  const Type *type = e->type->nonOptional();
  if (llvm::any_of(analysis_.aliasAnalysis().inside(type), holdsReferences)) {
    analysis_.codeWalk().touch(LocKind::Contents, Ref{RefKind::Type, type}, analysis_.inner_, write,
                               at);
  }
}

void ExprWalk::readSpread(const Expr *e, bool object, llvm::SMLoc at) {
  const Type *type = e && e->type ? e->type->nonOptional() : nullptr;
  if (!type) {
    return;
  }
  if (type->isIterator() || type->isAsyncIterator()) {
    analysis_.codeWalk().iterate(e, e);
    return;
  }
  if (object && type->isObject()) {
    analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(e), analysis_.star_,
                               false, at);
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
    Active before = analysis_.cur_;
    expr(a->value);
    merge(analysis_.cur_, before);
  };
  const Expr *target = a->target;
  if (auto *n = dyn_cast<NameExpr>(target)) {
    if (compound) {
      analysis_.codeWalk().var(n->var, false, n->loc());
    }
    value();
    analysis_.codeWalk().var(n->var, true, n->loc());
    return;
  }
  if (auto *m = dyn_cast<MemberExpr>(target)) {
    expr(m->object);
    if (compound) {
      fieldAccess(m->object, m->name, false, m->loc());
    }
    value();
    if (!analysis_.libraryEffects().librarySetter(m, a->value)) {
      fieldAccess(m->object, m->name, true, m->loc());
    }
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
    analysis_.codeWalk().var(n->var, false, n->loc());
    analysis_.codeWalk().var(n->var, true, n->loc());
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
  auto *c = dyn_cast<CallExpr>(strip(a->value));
  bool join = c && c->builtin == Builtin::PromiseAll;
  if (join) {
    analysis_.openJoins_.push_back({a, analysis_.cur_});
  }
  Use use;
  use.awaited = a;
  expr(a->value, use);
  analysis_.awaits_[a] = {analysis_.fi_};
  if (join) {
    Analysis::OpenJoin open = std::move(analysis_.openJoins_.back());
    analysis_.openJoins_.pop_back();
    merge(open.active, analysis_.cur_);
    Analysis::JoinInfo &info = analysis_.joins_[a];
    info.fn = analysis_.fi_;
    merge(info.segs, open.active);
  }
  analysis_.codeWalk().moveTo(analysis_.codeWalk().segment(a, SegKind::Await, a->loc()));
}

void ExprWalk::yieldExpr(const YieldExpr *y) {
  expr(y->value);
  if (y->delegate && y->value) {
    analysis_.codeWalk().iterate(y->value, y);
  }
  if (!analysis_.fn_->asyncGen) {
    return;
  }
  unsigned seg =
      analysis_.codeWalk().segment(y, y->delegate ? SegKind::Await : SegKind::Yield, y->loc());
  analysis_.fn_->segs[seg].delegate = y->delegate;
  analysis_.codeWalk().moveTo(seg);
}

}
