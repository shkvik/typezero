#include "compiler/parallel/ownership/ownership.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz::parallel {

Ownership::Ownership(Analysis &analysis) : analysis_(analysis) {}

std::optional<Ref> Ownership::ownedRef(const Expr *e) {
  const FunctionDecl *fn = analysis_.fn_ ? analysis_.fn_->decl : nullptr;
  const VarSymbol *root = rootOf(fn, e, 0);
  if (!root || !e->type) {
    return std::nullopt;
  }

  return interned(root, e->type->nonOptional());
}

Ref Ownership::interned(const VarSymbol *root, const Type *type) {
  auto key = std::make_pair(root, type);
  auto found = analysis_.ownedRefIndex_.find(key);
  if (found != analysis_.ownedRefIndex_.end()) {
    return Ref{RefKind::Owned, found->second};
  }
  OwnedRef &made = analysis_.ownedRefs_.emplace_back();
  made.root = root;
  made.type = type;
  analysis_.ownedRefIndex_[key] = &made;

  return Ref{RefKind::Owned, &made};
}

bool Ownership::isParam(const FunctionDecl *fn, const VarSymbol *v) {
  return fn && v && llvm::any_of(fn->params, [&](const Param &p) { return p.sym == v; });
}

bool Ownership::candidate(const FunctionDecl *fn, const VarSymbol *v) const {
  if (!fn || !v || fn->isGenerator || analysis_.isLibrary(fn)) {
    return false;
  }
  for (const Param &p : fn->params) {
    if (p.sym == v) {
      return !p.rest && !p.defaultValue && !p.property && !v->reassigned && !v->captured &&
             holdsReferences(v->type);
    }
  }

  return false;
}

bool Ownership::tracked(const FunctionDecl *fn, const VarSymbol *v) {
  return fn && v && v->owner == fn && !v->isGlobal && !v->captured && !v->reassigned;
}

bool Ownership::libraryField(const Type *object, llvm::StringRef name) {
  for (const ClassDecl *k = object->classDecl(); k; k = k->parent) {
    if (!k->library) {
      continue;
    }
    for (const ClassField &f : k->fields) {
      if (f.name == name && !f.isStatic) {
        return f.readonly || f.access != tz::Access::Public;
      }
    }
  }

  return false;
}

bool Ownership::plainValues(const Type *type, unsigned depth) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();
  if (!holdsReferences(type)) {
    return true;
  }
  if (depth > 8) {
    return false;
  }
  switch (type->kind()) {
  case Type::Kind::Promise:
    return plainValues(type->promised(), depth + 1);
  case Type::Kind::Result:
    return plainValues(type->okType(), depth + 1) && plainValues(type->errType(), depth + 1);
  case Type::Kind::Union:
    return llvm::all_of(type->members(),
                        [&](const Type *member) { return plainValues(member, depth + 1); });
  case Type::Kind::Object: {
    bool special = type->isClass() || type->isBytes() || type->isAnyObject() || type->isDate() ||
                   type->isRegExp() || type->isMatch() || type->isIntl() || type->isWeakRef() ||
                   type->isCrypto() || type->isRuntimeObject() || type->isTimer() ||
                   type->isIncomplete();
    if (special) {
      return false;
    }

    return llvm::all_of(type->fields(),
                        [&](const Field &f) { return plainValues(f.type, depth + 1); });
  }
  default:
    return false;
  }
}

bool Ownership::derivable(const Type *type, unsigned depth) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();
  if (plainValues(type, depth)) {
    return true;
  }
  if (depth > 8) {
    return false;
  }
  switch (type->kind()) {
  case Type::Kind::Result:
    return derivable(type->okType(), depth + 1) && derivable(type->errType(), depth + 1);
  case Type::Kind::Union:
    return llvm::all_of(type->members(),
                        [&](const Type *member) { return derivable(member, depth + 1); });
  case Type::Kind::Object: {
    const ClassDecl *cls = type->classDecl();

    return cls && cls->library;
  }
  default:
    return false;
  }
}

bool Ownership::valueArgument(const Expr *e) {
  e = strip(e);
  if (!e || !e->type) {
    return false;
  }
  if (e->type->nonOptional()->isFunction()) {
    return false;
  }
  if (!holdsReferences(e->type)) {
    return true;
  }
  if (auto *object = dyn_cast<ObjectLitExpr>(e)) {
    return llvm::all_of(object->props, [](const ObjectLitExpr::Property &p) {
      return !p.spread && !p.method && valueArgument(p.value);
    });
  }
  if (auto *array = dyn_cast<ArrayLitExpr>(e)) {
    return llvm::all_of(array->elements, [](const ArrayLitExpr::Element &el) {
      return !el.spread && (el.hole || valueArgument(el.expr));
    });
  }

  return false;
}

const VarSymbol *Ownership::rootOf(const FunctionDecl *fn, const Expr *e, unsigned depth) const {
  e = strip(e);
  if (!fn || !e || depth > 32) {
    return nullptr;
  }
  if (auto *n = dyn_cast<NameExpr>(e)) {
    const VarSymbol *v = n->var;
    if (isParam(fn, v)) {
      return candidate(fn, v) ? v : nullptr;
    }
    if (!tracked(fn, v)) {
      return nullptr;
    }
    auto found = analysis_.declInit_.find(v);
    if (found == analysis_.declInit_.end()) {
      return nullptr;
    }

    return rootOf(fn, found->second, depth + 1);
  }
  if (auto *m = dyn_cast<MemberExpr>(e)) {
    if (m->method || !m->object || !m->object->type) {
      return nullptr;
    }
    const Type *object = m->object->type->nonOptional();
    bool part = object->isResult() ? m->name == "value" || m->name == "error"
                                   : libraryField(object, m->name);
    if (!part || !derivable(m->type, 0)) {
      return nullptr;
    }

    return rootOf(fn, m->object, depth + 1);
  }
  if (auto *c = dyn_cast<CallExpr>(e)) {
    const FunctionDecl *lib = analysis_.libraryEffects().libraryTarget(c);
    auto *m = dyn_cast<MemberExpr>(c->callee);
    if (!lib || !lib->cls || lib->isStatic || lib->isAsync || lib->isGenerator || !c->method ||
        !m || c->hasSpread()) {
      return nullptr;
    }
    if (!llvm::all_of(c->args, valueArgument) || !derivable(c->type, 0)) {
      return nullptr;
    }

    return rootOf(fn, m->object, depth + 1);
  }

  return nullptr;
}

void Ownership::carriers(const Expr *e, Roots &out, unsigned depth) const {
  e = strip(e);
  if (!e || depth > 64 || !holdsReferences(e->type)) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::Name: {
    const VarSymbol *v = cast<NameExpr>(e)->var;
    if (isParam(fn_, v)) {
      if (candidate(fn_, v) && !llvm::is_contained(out, v)) {
        out.push_back(v);
      }
      return;
    }
    if (!tracked(fn_, v)) {
      return;
    }
    if (auto loop = loopInit_.find(v); loop != loopInit_.end()) {
      carriers(loop->second, out, depth + 1);
      return;
    }
    if (auto init = analysis_.declInit_.find(v); init != analysis_.declInit_.end()) {
      carriers(init->second, out, depth + 1);
    }
    return;
  }
  case Expr::Kind::Member:
    carriers(cast<MemberExpr>(e)->object, out, depth + 1);
    return;
  case Expr::Kind::Index:
    carriers(cast<IndexExpr>(e)->object, out, depth + 1);
    return;
  case Expr::Kind::Call: {
    auto *c = cast<CallExpr>(e);
    if (analysis_.libraryEffects().libraryTarget(c) && plainValues(c->type, 0)) {
      return;
    }
    if (auto *m = dyn_cast<MemberExpr>(c->callee)) {
      carriers(m->object, out, depth + 1);
    }
    for (const Expr *arg : c->args) {
      carriers(arg, out, depth + 1);
    }
    return;
  }
  case Expr::Kind::New: {
    auto *n = cast<NewExpr>(e);
    for (const Expr *arg : n->ctor ? n->ctor->args : n->args) {
      carriers(arg, out, depth + 1);
    }
    return;
  }
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &el : cast<ArrayLitExpr>(e)->elements) {
      if (!el.hole) {
        carriers(el.expr, out, depth + 1);
      }
    }
    return;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
      if (!p.method) {
        carriers(p.value, out, depth + 1);
      }
    }
    return;
  case Expr::Kind::Conditional:
    carriers(cast<ConditionalExpr>(e)->thenExpr, out, depth + 1);
    carriers(cast<ConditionalExpr>(e)->elseExpr, out, depth + 1);
    return;
  case Expr::Kind::Binary:
    carriers(cast<BinaryExpr>(e)->lhs, out, depth + 1);
    carriers(cast<BinaryExpr>(e)->rhs, out, depth + 1);
    return;
  case Expr::Kind::Assign:
    carriers(cast<AssignExpr>(e)->value, out, depth + 1);
    return;
  case Expr::Kind::Sequence: {
    auto *s = cast<SequenceExpr>(e);
    if (!s->exprs.empty()) {
      carriers(s->exprs.back(), out, depth + 1);
    }
    return;
  }
  case Expr::Kind::Cast:
    carriers(cast<CastExpr>(e)->operand, out, depth + 1);
    return;
  case Expr::Kind::OptionalChain:
    carriers(cast<OptionalChainExpr>(e)->chain, out, depth + 1);
    return;
  case Expr::Kind::Await:
    carriers(cast<AwaitExpr>(e)->value, out, depth + 1);
    return;
  case Expr::Kind::Unary:
    carriers(cast<UnaryExpr>(e)->operand, out, depth + 1);
    return;
  default:
    return;
  }
}

void Ownership::leak(const Expr *e) {
  Roots roots;
  carriers(e, roots, 0);
  for (const VarSymbol *v : roots) {
    leaked_.insert(v);
  }
}

void Ownership::visitStmt(const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *x : cast<BlockStmt>(s)->body) {
      visitStmt(x);
    }
    return;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls) {
      if (d.alias || !d.init) {
        continue;
      }
      visit(d.init);
      if (!tracked(fn_, d.sym)) {
        leak(d.init);
      }
    }
    return;
  case Stmt::Kind::Expr:
    visit(cast<ExprStmt>(s)->expr);
    return;
  case Stmt::Kind::If: {
    auto *i = cast<IfStmt>(s);
    visit(i->cond);
    visitStmt(i->thenStmt);
    visitStmt(i->elseStmt);
    return;
  }
  case Stmt::Kind::While:
    visit(cast<WhileStmt>(s)->cond);
    visitStmt(cast<WhileStmt>(s)->body);
    return;
  case Stmt::Kind::DoWhile:
    visitStmt(cast<DoWhileStmt>(s)->body);
    visit(cast<DoWhileStmt>(s)->cond);
    return;
  case Stmt::Kind::For: {
    auto *f = cast<ForStmt>(s);
    visitStmt(f->init);
    visit(f->cond);
    visit(f->step);
    visitStmt(f->body);
    return;
  }
  case Stmt::Kind::ForOf: {
    auto *f = cast<ForOfStmt>(s);
    visit(f->iterable);
    if (tracked(fn_, f->binding.sym)) {
      loopInit_[f->binding.sym] = f->iterable;
    } else {
      leak(f->iterable);
    }
    visitStmt(f->body);
    return;
  }
  case Stmt::Kind::Switch: {
    auto *sw = cast<SwitchStmt>(s);
    visit(sw->discriminant);
    for (const SwitchCase &c : sw->cases) {
      visit(c.test);
      for (const Stmt *x : c.body) {
        visitStmt(x);
      }
    }
    return;
  }
  case Stmt::Kind::Labeled:
    visitStmt(cast<LabeledStmt>(s)->body);
    return;
  case Stmt::Kind::Return:
    visit(cast<ReturnStmt>(s)->value);
    leak(cast<ReturnStmt>(s)->value);
    return;
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(s);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic && f.init) {
        visit(f.init);
        leak(f.init);
      }
    }
    for (const Stmt *x : cls->decorations) {
      visitStmt(x);
    }
    return;
  }
  default:
    return;
  }
}

void Ownership::visit(const Expr *e) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::Template:
    for (const Expr *x : cast<TemplateExpr>(e)->exprs) {
      visit(x);
    }
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &el : cast<ArrayLitExpr>(e)->elements) {
      if (!el.hole) {
        visit(el.expr);
      }
    }
    return;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
      visit(p.key);
      if (!p.method) {
        visit(p.value);
      }
    }
    return;
  case Expr::Kind::Member: {
    auto *m = cast<MemberExpr>(e);
    visit(m->object);
    if (m->method) {
      leak(m->object);
    }
    return;
  }
  case Expr::Kind::Index:
    visit(cast<IndexExpr>(e)->object);
    visit(cast<IndexExpr>(e)->index);
    return;
  case Expr::Kind::Unary:
    visit(cast<UnaryExpr>(e)->operand);
    return;
  case Expr::Kind::Binary:
    visit(cast<BinaryExpr>(e)->lhs);
    visit(cast<BinaryExpr>(e)->rhs);
    return;
  case Expr::Kind::Assign:
  case Expr::Kind::Update: {
    const Expr *target =
        isa<AssignExpr>(e) ? cast<AssignExpr>(e)->target : cast<UpdateExpr>(e)->target;
    if (auto *m = dyn_cast<MemberExpr>(target)) {
      visit(m->object);
    } else if (auto *ix = dyn_cast<IndexExpr>(target)) {
      visit(ix->object);
      visit(ix->index);
    } else if (!isa<NameExpr>(target)) {
      visit(target);
    }
    if (auto *a = dyn_cast<AssignExpr>(e)) {
      visit(a->value);
      leak(a->value);
    }
    return;
  }
  case Expr::Kind::Call:
    visitCall(cast<CallExpr>(e));
    return;
  case Expr::Kind::New: {
    auto *n = cast<NewExpr>(e);
    for (const Expr *arg : n->ctor ? n->ctor->args : n->args) {
      visit(arg);
      leak(arg);
    }
    return;
  }
  case Expr::Kind::Conditional:
    visit(cast<ConditionalExpr>(e)->cond);
    visit(cast<ConditionalExpr>(e)->thenExpr);
    visit(cast<ConditionalExpr>(e)->elseExpr);
    return;
  case Expr::Kind::Cast:
    visit(cast<CastExpr>(e)->operand);
    return;
  case Expr::Kind::Sequence:
    for (const Expr *x : cast<SequenceExpr>(e)->exprs) {
      visit(x);
    }
    return;
  case Expr::Kind::OptionalChain:
    visit(cast<OptionalChainExpr>(e)->chain);
    return;
  case Expr::Kind::Yield:
    visit(cast<YieldExpr>(e)->value);
    leak(cast<YieldExpr>(e)->value);
    return;
  case Expr::Kind::Await:
    visit(cast<AwaitExpr>(e)->value);
    return;
  default:
    return;
  }
}

void Ownership::visitCall(const CallExpr *c) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  if (c->builtin != Builtin::None) {
    if (m) {
      visit(m->object);
    }
    bool reads = onlyReadsArgs(c->builtin);
    for (const Expr *arg : c->args) {
      visit(arg);
      if (!reads) {
        leak(arg);
      }
    }
    return;
  }
  if (analysis_.libraryEffects().libraryTarget(c)) {
    if (m && c->method) {
      visit(m->object);
    }
    for (const Expr *arg : c->args) {
      visit(arg);
      leak(arg);
    }
    return;
  }
  if (m && (c->method || !c->unionMethods.empty())) {
    visit(m->object);
    leak(m->object);
  } else if (!c->func) {
    visit(c->callee);
  }
  for (size_t i = 0; i < c->args.size(); ++i) {
    visit(c->args[i]);
    if (!passesOwned(c, i)) {
      leak(c->args[i]);
    }
  }
}

bool Ownership::passesOwned(const CallExpr *c, size_t index) const {
  if (c->hasSpread()) {
    return false;
  }
  const FnInfo &f = analysis_.fns_[fi_];
  auto found =
      f.callOf.find(std::make_pair(static_cast<const void *>(c), unsigned(CallKind::Call)));
  if (found == f.callOf.end()) {
    return false;
  }
  const CallRec &rec = f.calls[found->second];
  if (rec.reflect != CallRec::Reflect::None || rec.channel >= 0 || rec.callees.empty()) {
    return false;
  }
  for (const Callee &callee : rec.callees) {
    if (callee.mode != Mode::Sync && callee.mode != Mode::Await) {
      return false;
    }
    const FunctionDecl *g = analysis_.fns_[callee.fn].decl;
    if (!g || index >= g->params.size()) {
      return false;
    }
    const VarSymbol *param = g->params[index].sym;
    if (!candidate(g, param) || leaking_.contains(param)) {
      return false;
    }
  }

  return true;
}

bool Ownership::callersOwned(unsigned fi, size_t index) const {
  for (const Site &site : sites_[fi]) {
    const FnInfo &caller = analysis_.fns_[site.fn];
    const CallRec &rec = caller.calls[site.rec];
    for (const Callee &callee : rec.callees) {
      if (callee.fn != fi) {
        continue;
      }
      if (rec.served) {
        if (index < 2) {
          continue;
        }
        return false;
      }
      bool direct = rec.kind == CallKind::Call && rec.reflect == CallRec::Reflect::None &&
                    rec.channel < 0 && rec.call && !rec.call->hasSpread() &&
                    (callee.mode == Mode::Sync || callee.mode == Mode::Await);
      if (!direct || index >= rec.call->args.size()) {
        return false;
      }
      const VarSymbol *root = rootOf(caller.decl, rec.call->args[index], 0);
      if (!root || !analysis_.ownedParams_.contains(root)) {
        return false;
      }
    }
  }

  return true;
}

Ref Ownership::resolved(Ref r) const {
  if (r.kind != RefKind::Owned) {
    return r;
  }
  auto *owned = static_cast<const OwnedRef *>(r.ptr);
  if (analysis_.ownedParams_.contains(owned->root)) {
    return Ref{RefKind::Local, nullptr};
  }

  return Ref{RefKind::Type, owned->type};
}

Ref Ownership::substituted(Ref r, const FnInfo &callee, const FnInfo &caller, const CallRec &rec,
                           Mode mode) {
  if (r.kind != RefKind::Owned) {
    return r;
  }
  auto *owned = static_cast<const OwnedRef *>(r.ptr);
  Ref plain{RefKind::Type, owned->type};
  auto found = paramOf_.find(owned->root);
  if (found == paramOf_.end() || found->second.first != callee.decl ||
      leaking_.contains(owned->root)) {
    return plain;
  }
  bool direct = !rec.served && rec.kind == CallKind::Call &&
                rec.reflect == CallRec::Reflect::None && rec.channel < 0 && rec.call &&
                !rec.call->hasSpread() && (mode == Mode::Sync || mode == Mode::Await);
  unsigned index = found->second.second;
  if (!direct || index >= rec.call->args.size()) {
    return plain;
  }
  const VarSymbol *root = rootOf(caller.decl, rec.call->args[index], 0);
  if (!root) {
    return plain;
  }

  return interned(root, owned->type);
}

void Ownership::rewrite() {
  for (FnInfo &f : analysis_.fns_) {
    for (Segment &seg : f.segs) {
      bool any = llvm::any_of(seg.effects,
                              [](const Access &a) { return a.loc.ref.kind == RefKind::Owned; });
      if (!any) {
        continue;
      }
      Effects next;
      for (const Access &a : seg.effects) {
        if (a.loc.ref.kind != RefKind::Owned) {
          next.insert(a);
          continue;
        }
        auto *owned = static_cast<const OwnedRef *>(a.loc.ref.ptr);
        Access b = a;
        b.loc.ref = Ref{RefKind::Type, owned->type};
        if (!analysis_.ownedParams_.contains(owned->root)) {
          next.insert(b);
          continue;
        }
        bool inner = a.loc.kind == LocKind::Contents &&
                     llvm::any_of(analysis_.aliasAnalysis().inside(owned->type), holdsReferences);
        if (inner) {
          b.loc.key = analysis_.inner_;
          next.insert(b);
        }
      }
      seg.effects = std::move(next);
    }
    for (CallRec &rec : f.calls) {
      for (Callee &callee : rec.callees) {
        if (callee.self) {
          callee.self = resolved(*callee.self);
        }
      }
      if (rec.bySelf) {
        rec.bySelf = resolved(*rec.bySelf);
      }
    }
  }
}

void Ownership::scan(unsigned fi) {
  fi_ = fi;
  fn_ = analysis_.fns_[fi].decl;
  leaked_.clear();
  loopInit_.clear();
  for (const Param &p : fn_->params) {
    if (p.defaultValue) {
      visit(p.defaultValue);
      leak(p.defaultValue);
    }
  }
  visitStmt(fn_->body);
  fn_ = nullptr;
}

void Ownership::decide() {
  analysis_.ownedParams_.clear();
  leaking_.clear();
  paramOf_.clear();
  sites_.assign(analysis_.fns_.size(), {});
  for (unsigned fi = 0; fi < analysis_.fns_.size(); ++fi) {
    const FnInfo &f = analysis_.fns_[fi];
    for (unsigned r = 0; r < f.calls.size(); ++r) {
      for (const Callee &callee : f.calls[r].callees) {
        sites_[callee.fn].push_back({fi, r});
      }
    }
  }
  std::vector<unsigned> scanned;
  for (unsigned fi = 0; fi < analysis_.fns_.size(); ++fi) {
    const FunctionDecl *decl = analysis_.fns_[fi].decl;
    if (!decl) {
      continue;
    }
    bool any = false;
    for (unsigned i = 0; i < decl->params.size(); ++i) {
      const VarSymbol *v = decl->params[i].sym;
      if (candidate(decl, v)) {
        paramOf_[v] = {decl, i};
        any = true;
      }
    }
    if (any) {
      scanned.push_back(fi);
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (unsigned fi : scanned) {
      scan(fi);
      for (const VarSymbol *v : leaked_) {
        changed |= leaking_.insert(v).second;
      }
    }
  }
  for (const auto &[v, place] : paramOf_) {
    if (!leaking_.contains(v)) {
      analysis_.ownedParams_.insert(v);
    }
  }
  changed = true;
  while (changed) {
    changed = false;
    for (unsigned fi : scanned) {
      const FunctionDecl *decl = analysis_.fns_[fi].decl;
      for (size_t i = 0; i < decl->params.size(); ++i) {
        const VarSymbol *v = decl->params[i].sym;
        if (v && analysis_.ownedParams_.contains(v) && !callersOwned(fi, i)) {
          analysis_.ownedParams_.erase(v);
          changed = true;
        }
      }
    }
  }
}

}
