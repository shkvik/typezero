#include "compiler/parallel/escape_scan/escape_scan.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
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

namespace tz::parallel {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

namespace {

bool keepsReceiver(Builtin b, bool discarded) {
  switch (b) {
  case Builtin::ArrReverse:
  case Builtin::ArrFill:
  case Builtin::ArrSort:
  case Builtin::ArrSortWith:
  case Builtin::MapSet:
  case Builtin::SetAdd:
  case Builtin::BytesReverse:
  case Builtin::BytesSort:
  case Builtin::BytesFill:
  case Builtin::BytesCopyWithin:
  case Builtin::BufferFill:
  case Builtin::BufferSwap:
    return discarded;
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries:
  case Builtin::BytesSubarray:
  case Builtin::BytesBuffer:
  case Builtin::BytesView:
    return false;
  default:
    return knownBuiltin(b);
  }
}

}

EscapeScan::EscapeScan(Analysis &analysis) : analysis_(analysis) {}

const Analysis::Escapes &EscapeScan::escapes(const FunctionDecl *owner) {
  auto found = analysis_.escapes_.find(owner);
  if (found != analysis_.escapes_.end()) {
    return found->second;
  }
  Analysis::Escapes out;
  if (owner) {
    for (const Param &p : owner->params) {
      scanUses(p.defaultValue, false, out);
    }
    scanStmt(owner->body, out);
  } else {
    for (const Stmt *item : analysis_.program_.items) {
      if (!isa<FunctionDecl, ClassDecl>(item)) {
        scanStmt(item, out);
      }
    }
  }

  return analysis_.escapes_[owner] = std::move(out);
}

void EscapeScan::scanStmt(const Stmt *s, Analysis::Escapes &out) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *x : cast<BlockStmt>(s)->body) {
      scanStmt(x, out);
    }
    return;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls) {
      scanUses(d.init, false, out);
    }
    return;
  case Stmt::Kind::Expr:
    scanUses(cast<ExprStmt>(s)->expr, true, out, true);
    return;
  case Stmt::Kind::If:
    scanUses(cast<IfStmt>(s)->cond, true, out);
    scanStmt(cast<IfStmt>(s)->thenStmt, out);
    scanStmt(cast<IfStmt>(s)->elseStmt, out);
    return;
  case Stmt::Kind::While:
    scanUses(cast<WhileStmt>(s)->cond, true, out);
    scanStmt(cast<WhileStmt>(s)->body, out);
    return;
  case Stmt::Kind::DoWhile:
    scanStmt(cast<DoWhileStmt>(s)->body, out);
    scanUses(cast<DoWhileStmt>(s)->cond, true, out);
    return;
  case Stmt::Kind::For: {
    auto *f = cast<ForStmt>(s);
    scanStmt(f->init, out);
    scanUses(f->cond, true, out);
    scanUses(f->step, true, out, true);
    scanStmt(f->body, out);
    return;
  }
  case Stmt::Kind::ForOf:
    scanUses(cast<ForOfStmt>(s)->iterable, true, out);
    scanStmt(cast<ForOfStmt>(s)->body, out);
    return;
  case Stmt::Kind::Switch:
    scanUses(cast<SwitchStmt>(s)->discriminant, true, out);
    for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
      scanUses(c.test, true, out);
      for (const Stmt *x : c.body) {
        scanStmt(x, out);
      }
    }
    return;
  case Stmt::Kind::Labeled:
    scanStmt(cast<LabeledStmt>(s)->body, out);
    return;
  case Stmt::Kind::Return:
    scanUses(cast<ReturnStmt>(s)->value, false, out);
    return;
  default:
    return;
  }
}

void EscapeScan::scanUses(const Expr *e, bool safe, Analysis::Escapes &out, bool discarded) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::Name:
    if (!safe && cast<NameExpr>(e)->var) {
      out.insert(cast<NameExpr>(e)->var);
    }
    return;
  case Expr::Kind::Member: {
    auto *m = cast<MemberExpr>(e);
    scanUses(m->object, !m->method, out);
    return;
  }
  case Expr::Kind::Index:
    scanUses(cast<IndexExpr>(e)->object, true, out);
    scanUses(cast<IndexExpr>(e)->index, false, out);
    return;
  case Expr::Kind::Cast:
    scanUses(cast<CastExpr>(e)->operand, cast<CastExpr>(e)->isImplicit() && safe, out, discarded);
    return;
  case Expr::Kind::Unary: {
    auto *u = cast<UnaryExpr>(e);
    scanUses(u->operand, true, out, false);
    return;
  }
  case Expr::Kind::Binary: {
    auto *b = cast<BinaryExpr>(e);
    bool flows =
        b->op == BinaryOp::LogAnd || b->op == BinaryOp::LogOr || b->op == BinaryOp::Nullish;
    scanUses(b->lhs, flows ? safe : true, out);
    scanUses(b->rhs, flows ? safe : true, out);
    return;
  }
  case Expr::Kind::Assign: {
    auto *a = cast<AssignExpr>(e);
    if (auto *m = dyn_cast<MemberExpr>(a->target)) {
      scanUses(m->object, true, out);
    } else if (auto *ix = dyn_cast<IndexExpr>(a->target)) {
      scanUses(ix->object, true, out);
      scanUses(ix->index, false, out);
    }
    scanUses(a->value, false, out);
    return;
  }
  case Expr::Kind::Update: {
    const Expr *t = cast<UpdateExpr>(e)->target;
    if (auto *m = dyn_cast<MemberExpr>(t)) {
      scanUses(m->object, true, out);
    } else if (auto *ix = dyn_cast<IndexExpr>(t)) {
      scanUses(ix->object, true, out);
      scanUses(ix->index, false, out);
    }
    return;
  }
  case Expr::Kind::Call: {
    auto *c = cast<CallExpr>(e);
    auto *m = dyn_cast<MemberExpr>(c->callee);
    if (c->builtin != Builtin::None) {
      if (m) {
        scanUses(m->object, keepsReceiver(c->builtin, discarded), out);
      }
      bool reads = onlyReadsArgs(c->builtin);
      for (const Expr *arg : c->args) {
        scanUses(arg, reads, out);
      }
      return;
    }
    if (m && c->method && analysis_.isLibrary(c->method->decl)) {
      scanUses(m->object, true, out);
    } else if (m && (c->method || !c->unionMethods.empty())) {
      scanUses(m->object, false, out);
    } else {
      scanUses(c->callee, true, out);
    }
    for (const Expr *arg : c->args) {
      scanUses(arg, false, out);
    }
    return;
  }
  case Expr::Kind::New: {
    auto *n = cast<NewExpr>(e);
    for (const Expr *arg : n->ctor ? n->ctor->args : n->args) {
      scanUses(arg, false, out);
    }
    return;
  }
  case Expr::Kind::Conditional:
    scanUses(cast<ConditionalExpr>(e)->cond, true, out);
    scanUses(cast<ConditionalExpr>(e)->thenExpr, safe, out);
    scanUses(cast<ConditionalExpr>(e)->elseExpr, safe, out);
    return;
  case Expr::Kind::Sequence: {
    auto *s = cast<SequenceExpr>(e);
    for (size_t i = 0; i < s->exprs.size(); ++i) {
      scanUses(s->exprs[i], i + 1 < s->exprs.size() || safe, out);
    }
    return;
  }
  case Expr::Kind::OptionalChain:
    scanUses(cast<OptionalChainExpr>(e)->chain, safe, out, discarded);
    return;
  case Expr::Kind::Template:
    for (const Expr *x : cast<TemplateExpr>(e)->exprs) {
      scanUses(x, true, out);
    }
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &el : cast<ArrayLitExpr>(e)->elements) {
      scanUses(el.expr, el.spread, out);
    }
    return;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
      scanUses(p.key, true, out);
      if (!p.method) {
        scanUses(p.value, p.spread, out);
      }
    }
    return;
  case Expr::Kind::Await:
    scanUses(cast<AwaitExpr>(e)->value, false, out);
    return;
  case Expr::Kind::Yield:
    scanUses(cast<YieldExpr>(e)->value, false, out);
    return;
  default:
    return;
  }
}

bool EscapeScan::constructorLeaks(const ClassDecl *cls) {
  auto found = analysis_.leaks_.find(cls);
  if (found != analysis_.leaks_.end()) {
    return found->second;
  }
  analysis_.leaks_[cls] = true;
  bool leaks = false;
  for (const ClassDecl *k = cls; k && !leaks; k = k->parent) {
    std::vector<const FunctionDecl *> fns(k->constructors.begin(), k->constructors.end());
    if (k->init) {
      fns.push_back(k->init);
    }
    for (const FunctionDecl *fn : fns) {
      if (!fn->thisVar) {
        continue;
      }
      if (fn->thisVar->captured || escapes(fn).contains(fn->thisVar)) {
        leaks = true;
        break;
      }
    }
  }

  return analysis_.leaks_[cls] = leaks;
}

}
