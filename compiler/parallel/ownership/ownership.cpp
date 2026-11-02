#include "compiler/parallel/ownership/ownership.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/escape_scan/escape_scan.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

Ownership::Ownership(Analysis &analysis) : analysis_(analysis) {}

std::optional<Ref> Ownership::ownedRef(const Expr *e) {
  const FunctionDecl *fn = analysis_.fn_ ? analysis_.fn_->decl : nullptr;
  Needs needs;
  const VarSymbol *root = rootOf(fn, e, needs, 0);
  if (!root || !e->type) {
    return std::nullopt;
  }
  auto *name = dyn_cast<NameExpr>(strip(e));
  bool whole = needs.empty() && name && name->var == root;

  return interned(root, e->type->nonOptional(), std::move(needs), whole);
}

Ref Ownership::interned(const VarSymbol *root, const Type *type, Needs needs, bool whole) {
  llvm::sort(needs);
  needs.erase(std::unique(needs.begin(), needs.end()), needs.end());
  auto key = std::make_tuple(root, type, needs, whole);
  auto found = analysis_.ownedRefIndex_.find(key);
  if (found != analysis_.ownedRefIndex_.end()) {
    return Ref{RefKind::Owned, found->second};
  }
  OwnedRef &made = analysis_.ownedRefs_.emplace_back();
  made.root = root;
  made.type = type;
  made.needs = needs;
  made.whole = whole;
  made.fallback = Ref{RefKind::Type, type};
  if (whole) {
    auto init = analysis_.declInit_.find(root);
    if (init != analysis_.declInit_.end() && analysis_.aliasAnalysis().isAllocation(init->second)) {
      made.fallback = Ref{RefKind::Site, init->second};
    }
  }
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

bool Ownership::freshCandidate(const FunctionDecl *fn, const VarSymbol *v) const {
  if (!fn || fn->isGenerator || analysis_.isLibrary(fn) || !tracked(fn, v) ||
      !holdsReferences(v->type)) {
    return false;
  }
  auto init = analysis_.declInit_.find(v);
  if (init == analysis_.declInit_.end()) {
    return false;
  }

  return freshAllocation(init->second) || programCall(init->second);
}

bool Ownership::freshAllocation(const Expr *e) const {
  e = strip(e);
  if (!e || !analysis_.aliasAnalysis().isAllocation(e)) {
    return false;
  }
  auto *n = dyn_cast<NewExpr>(e);

  return !(n && n->cls && analysis_.escapeScan().constructorLeaks(n->cls));
}

const CallExpr *Ownership::programCall(const Expr *e) const {
  e = strip(e);
  if (auto *a = dyn_cast_or_null<AwaitExpr>(e)) {
    e = strip(a->value);
  }
  auto *c = dyn_cast_or_null<CallExpr>(e);
  if (!c || c->builtin != Builtin::None || analysis_.libraryEffects().libraryTarget(c)) {
    return nullptr;
  }

  return c;
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

bool Ownership::plainObject(const Type *type) {
  if (!type || type->kind() != Type::Kind::Object || type->classDecl() || type->isTuple()) {
    return false;
  }
  bool special = type->isClass() || type->isBytes() || type->isAnyObject() || type->isDate() ||
                 type->isRegExp() || type->isMatch() || type->isIntl() || type->isWeakRef() ||
                 type->isCrypto() || type->isRuntimeObject() || type->isTimer() ||
                 type->isIncomplete();

  return !special;
}

bool Ownership::container(const Type *type) {
  if (!containers_) {
    containers_.emplace();
    std::set<const Type *> library;
    for (const Stmt *item : analysis_.program_.items) {
      if (analysis_.libraryEffects().inLibrary(item->loc())) {
        libraryTypes(item, library);
        continue;
      }
      auto *alias = dyn_cast<TypeAliasDecl>(item);
      if (alias && alias->resolved && alias->typeParams.empty() &&
          plainObject(alias->resolved->nonOptional())) {
        containers_->insert(alias->resolved->nonOptional());
      }
    }
    std::vector<const Type *> known(containers_->begin(), containers_->end());
    for (const Type *t : known) {
      for (const Type *other : library) {
        if (other->kind() == Type::Kind::Object && !other->isAnyObject() &&
            analysis_.aliasAnalysis().objectTypesMayAlias(other, t)) {
          containers_->erase(t);
          break;
        }
      }
    }
    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<const Type *> known(containers_->begin(), containers_->end());
      for (const Type *t : known) {
        bool foreign = false;
        for (const auto &[from, to] : analysis_.program_.views) {
          const Type *other = from == t ? to : to == t ? from : nullptr;
          foreign |= other && !containers_->contains(other);
        }
        for (const Type *sub : analysis_.program_.subtypes) {
          bool related = sub != t && (sub->isSubtypeOf(t) || t->isSubtypeOf(sub));
          foreign |= related && !containers_->contains(sub);
        }
        if (foreign) {
          containers_->erase(t);
          changed = true;
        }
      }
    }
  }

  return type && containers_->contains(type);
}

void Ownership::libraryTypes(const Stmt *item, std::set<const Type *> &out) {
  std::function<void(const Type *)> add = [&](const Type *t) {
    if (!t || !out.insert(t).second) {
      return;
    }
    switch (t->kind()) {
    case Type::Kind::Optional:
    case Type::Kind::Array:
    case Type::Kind::Promise:
    case Type::Kind::Iterator:
      add(t->inner());
      return;
    case Type::Kind::Union:
      for (const Type *member : t->members()) {
        add(member);
      }
      return;
    case Type::Kind::Dict:
      add(t->keyType());
      add(t->valueType());
      return;
    case Type::Kind::Result:
      add(t->okType());
      add(t->errType());
      return;
    case Type::Kind::Function:
      for (const Type *param : t->params()) {
        add(param);
      }
      add(t->ret());
      return;
    case Type::Kind::Object:
      for (const Field &f : t->fields()) {
        add(f.type);
      }
      return;
    default:
      return;
    }
  };
  auto signature = [&](const FunctionDecl *fn) {
    if (!fn || !fn->typeParams.empty() || !fn->sym || fn->sym->isGeneric()) {
      return;
    }
    for (const Type *param : fn->sym->params) {
      add(param);
    }
    add(fn->sym->ret);
  };
  if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
    if (alias->typeParams.empty()) {
      add(alias->resolved);
    }
  } else if (auto *fn = dyn_cast<FunctionDecl>(item)) {
    signature(fn);
  } else if (auto *cls = dyn_cast<ClassDecl>(item)) {
    if (cls->generic) {
      return;
    }
    for (const ClassField &f : cls->fields) {
      add(f.resolved);
    }
    for (const FunctionDecl *method : cls->methods) {
      signature(method);
    }
    for (const FunctionDecl *ctor : cls->constructors) {
      signature(ctor);
    }
  } else if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
    for (const VarDeclarator &d : decl->decls) {
      if (d.sym) {
        add(d.sym->type);
      }
    }
  }
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

const VarSymbol *Ownership::rootOf(const FunctionDecl *fn, const Expr *e, Needs &needs,
                                   unsigned depth) {
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
    if (freshCandidate(fn, v)) {
      return v;
    }
    auto found = analysis_.declInit_.find(v);
    if (found == analysis_.declInit_.end()) {
      return nullptr;
    }

    return rootOf(fn, found->second, needs, depth + 1);
  }
  if (auto *m = dyn_cast<MemberExpr>(e)) {
    if (m->method || !m->object || !m->object->type || !m->type) {
      return nullptr;
    }
    const Type *object = m->object->type->nonOptional();
    bool part = object->isResult() ? m->name == "value" || m->name == "error"
                                   : libraryField(object, m->name);
    if (part) {
      return derivable(m->type, 0) ? rootOf(fn, m->object, needs, depth + 1) : nullptr;
    }
    if (m->type->nonOptional()->isFunction() || !object->field(m->name) || !container(object)) {
      return nullptr;
    }
    FieldKey key{object, analysis_.intern(m->name)};
    needs.push_back(key);
    asked_.insert(key);

    return rootOf(fn, m->object, needs, depth + 1);
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

    return rootOf(fn, m->object, needs, depth + 1);
  }

  return nullptr;
}

void Ownership::carriers(const Expr *e, Roots &out, unsigned depth) {
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
    if (freshCandidate(fn_, v) && !llvm::is_contained(out, v)) {
      out.push_back(v);
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
    if (analysis_.libraryEffects().libraryTarget(c) &&
        (plainValues(c->type, 0) || analysis_.aliasAnalysis().isAllocation(c))) {
      return;
    }
    if (auto *m = dyn_cast<MemberExpr>(c->callee)) {
      carriers(m->object, out, depth + 1);
    }
    bool program = c->builtin == Builtin::None && !analysis_.libraryEffects().libraryTarget(c);
    for (size_t i = 0; i < c->args.size(); ++i) {
      if (!program || !passesOwned(c, i)) {
        carriers(c->args[i], out, depth + 1);
      }
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
    if (!analysis_.aliasAnalysis().isAllocation(e)) {
      carriers(cast<AwaitExpr>(e)->value, out, depth + 1);
    }
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

void Ownership::leakParams(const Expr *e) {
  Roots roots;
  carriers(e, roots, 0);
  for (const VarSymbol *v : roots) {
    if (isParam(fn_, v)) {
      leaked_.insert(v);
    }
  }
}

void Ownership::literal(const ObjectLitExpr *o) {
  const Type *type = o->type ? o->type->nonOptional() : nullptr;
  if (!type) {
    return;
  }
  for (const ObjectLitExpr::Property &p : o->props) {
    if (p.method) {
      continue;
    }
    if (p.spread || p.key) {
      dirty_[analysis_.star_].push_back(type);
      continue;
    }
    stores_[analysis_.intern(p.name)].push_back(
        {type, p.value, fi_, o == heldLiteral_ ? holder_ : nullptr});
  }
}

void Ownership::dirty(const Expr *object, llvm::StringRef field) {
  const Type *type = object && object->type ? object->type->nonOptional() : nullptr;
  if (!type) {
    return;
  }
  const char *key = analysis_.intern(field);
  if (type->isUnion()) {
    for (const Type *member : type->members()) {
      dirty_[key].push_back(member->nonOptional());
    }
    return;
  }
  dirty_[key].push_back(type);
}

void Ownership::dirtyAll(const Expr *e) {
  if (!e || !e->type || !holdsReferences(e->type)) {
    return;
  }
  const Type *type = e->type->nonOptional();
  std::vector<const Type *> &all = dirty_[analysis_.star_];
  all.push_back(type);
  for (const Type *inner : analysis_.aliasAnalysis().inside(type)) {
    all.push_back(inner->nonOptional());
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
      heldLiteral_ = strip(d.init);
      holder_ = d.sym;
      visit(d.init);
      heldLiteral_ = nullptr;
      holder_ = nullptr;
      if (!tracked(fn_, d.sym)) {
        leak(d.init);
      } else if (collecting_ && freshCandidate(fn_, d.sym)) {
        freshOf_[d.sym] = fi_;
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
  case Stmt::Kind::Return: {
    const Expr *value = cast<ReturnStmt>(s)->value;
    visit(value);
    leakParams(value);
    if (collecting_ && value) {
      returns_[fi_].push_back(value);
    }
    return;
  }
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
  case Expr::Kind::ObjectLit: {
    auto *o = cast<ObjectLitExpr>(e);
    if (collecting_) {
      literal(o);
    }
    for (const ObjectLitExpr::Property &p : o->props) {
      visit(p.key);
      if (!p.method) {
        visit(p.value);
      }
    }
    return;
  }
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
  case Expr::Kind::Unary: {
    auto *u = cast<UnaryExpr>(e);
    if (collecting_ && u->op == UnaryOp::Delete) {
      if (auto *m = dyn_cast<MemberExpr>(strip(u->operand))) {
        dirty(m->object, m->name);
      }
    }
    visit(u->operand);
    return;
  }
  case Expr::Kind::Binary:
    visit(cast<BinaryExpr>(e)->lhs);
    visit(cast<BinaryExpr>(e)->rhs);
    return;
  case Expr::Kind::Assign:
  case Expr::Kind::Update: {
    const Expr *target =
        isa<AssignExpr>(e) ? cast<AssignExpr>(e)->target : cast<UpdateExpr>(e)->target;
    if (auto *m = dyn_cast<MemberExpr>(target)) {
      if (collecting_) {
        dirty(m->object, m->name);
      }
      visit(m->object);
    } else if (auto *ix = dyn_cast<IndexExpr>(target)) {
      if (collecting_ && !ix->field.empty()) {
        dirty(ix->object, ix->field);
      } else if (collecting_ && ix->keyed) {
        dirty(ix->object, "*");
      }
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
    if (collecting_ && !knownBuiltin(c->builtin)) {
      dirtyAll(m ? m->object : nullptr);
      for (const Expr *arg : c->args) {
        dirtyAll(arg);
      }
    }
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

bool Ownership::thisFirst(const CallRec &rec) const {
  const CallExpr *c = rec.call;
  if (!c || c->method || c->func || !c->unionMethods.empty() || !c->callee || !c->callee->type) {
    return false;
  }
  const Type *type = c->callee->type->nonOptional();

  return type->isFunction() && type->hasThis();
}

bool Ownership::tailAwaited(unsigned fi) {
  if (tail_[fi] == 1) {
    return false;
  }
  if (tail_[fi] > 1) {
    return tail_[fi] == 2;
  }
  const FnInfo &f = analysis_.fns_[fi];
  if (!f.decl || f.async || f.asyncGen || f.syncGen) {
    tail_[fi] = 3;
    return false;
  }
  tail_[fi] = 1;
  bool awaited = !sites_[fi].empty();
  for (const Site &site : sites_[fi]) {
    const FnInfo &caller = analysis_.fns_[site.fn];
    const CallRec &rec = caller.calls[site.rec];
    if (rec.served) {
      continue;
    }
    bool direct = rec.kind == CallKind::Call && rec.reflect == CallRec::Reflect::None &&
                  rec.channel < 0 && rec.call && rec.call->builtin == Builtin::None;
    bool waits = rec.await || (rec.returned && caller.async) || (rec.tail && tailAwaited(site.fn));
    if (!direct || !waits) {
      awaited = false;
      break;
    }
  }
  tail_[fi] = awaited ? 2 : 3;

  return awaited;
}

bool Ownership::handsOver(unsigned fi, const CallRec &rec, Mode mode, bool strict) {
  if (mode == Mode::Sync || mode == Mode::Await) {
    return true;
  }

  return mode == Mode::Spawn && !strict && rec.tail && tailAwaited(fi);
}

int Ownership::argOf(unsigned fi, const CallRec &rec, const Callee &callee, size_t param,
                     bool strict) {
  if (rec.served || rec.kind != CallKind::Call || rec.reflect != CallRec::Reflect::None ||
      rec.channel >= 0 || !rec.call || rec.call->builtin != Builtin::None ||
      rec.call->hasSpread() || !handsOver(fi, rec, callee.mode, strict)) {
    return -1;
  }
  size_t index = param + (thisFirst(rec) ? 1 : 0);

  return index < rec.call->args.size() ? int(index) : -1;
}

bool Ownership::passesOwned(const CallExpr *c, size_t index) {
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
  size_t shift = thisFirst(rec) ? 1 : 0;
  if (index < shift) {
    return false;
  }
  for (const Callee &callee : rec.callees) {
    if (!handsOver(fi_, rec, callee.mode, false)) {
      return false;
    }
    const FunctionDecl *g = analysis_.fns_[callee.fn].decl;
    if (!g || index - shift >= g->params.size()) {
      return false;
    }
    const VarSymbol *param = g->params[index - shift].sym;
    if (!candidate(g, param) || leaking_.contains(param)) {
      return false;
    }
  }

  return true;
}

bool Ownership::callersOwned(unsigned fi, size_t index, bool strict) {
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
      int arg = argOf(site.fn, rec, callee, index, strict);
      if (arg < 0 || !valueOwn(site.fn, rec.call->args[size_t(arg)], true, 0)) {
        return false;
      }
    }
  }

  return true;
}

bool Ownership::callFresh(unsigned fi, const CallExpr *c) const {
  const FnInfo &f = analysis_.fns_[fi];
  auto found =
      f.callOf.find(std::make_pair(static_cast<const void *>(c), unsigned(CallKind::Call)));
  if (found == f.callOf.end()) {
    return false;
  }
  const CallRec &rec = f.calls[found->second];
  if (rec.reflect != CallRec::Reflect::None || rec.channel >= 0 || rec.callees.empty()) {
    return false;
  }

  return llvm::all_of(rec.callees, [&](const Callee &callee) {
    return (callee.mode == Mode::Sync || callee.mode == Mode::Await) && fresh_.contains(callee.fn);
  });
}

bool Ownership::valueOwn(unsigned fi, const Expr *e, bool params, unsigned depth) {
  e = strip(e);
  if (!e || !e->type || depth > 16) {
    return false;
  }
  if (!holdsReferences(e->type)) {
    return true;
  }
  if (e->type->nonOptional()->isFunction()) {
    return false;
  }
  if (auto *c = dyn_cast<ConditionalExpr>(e)) {
    return valueOwn(fi, c->thenExpr, params, depth + 1) &&
           valueOwn(fi, c->elseExpr, params, depth + 1);
  }
  if (auto *b = dyn_cast<BinaryExpr>(e);
      b && (b->op == BinaryOp::LogAnd || b->op == BinaryOp::LogOr || b->op == BinaryOp::Nullish)) {
    return valueOwn(fi, b->lhs, params, depth + 1) && valueOwn(fi, b->rhs, params, depth + 1);
  }
  const FunctionDecl *fn = analysis_.fns_[fi].decl;
  Needs needs;
  if (const VarSymbol *root = rootOf(fn, e, needs, 0)) {
    if (!params && isParam(fn, root)) {
      return false;
    }
    return owned_.contains(root) && needsOwn(needs);
  }
  if (freshAllocation(e)) {
    return true;
  }
  if (const CallExpr *c = programCall(e)) {
    return callFresh(fi, c);
  }

  return false;
}

bool Ownership::fieldOpen(const FieldKey &key) {
  auto [found, inserted] = open_.try_emplace(key, false);
  if (!inserted) {
    return found->second;
  }
  if (!container(key.first)) {
    return false;
  }
  for (const char *field : {key.second, analysis_.star_}) {
    auto types = dirty_.find(field);
    if (types == dirty_.end()) {
      continue;
    }
    for (const Type *type : types->second) {
      if (analysis_.aliasAnalysis().typesMayAlias(type, key.first)) {
        return false;
      }
    }
  }

  return open_[key] = true;
}

bool Ownership::fieldOwn(const FieldKey &key) {
  if (!fieldOpen(key)) {
    return false;
  }
  auto stores = stores_.find(key.second);
  if (stores == stores_.end()) {
    return true;
  }
  for (const Store &s : stores->second) {
    if ((s.holder && !owned_.contains(s.holder)) ||
        !analysis_.aliasAnalysis().typesMayAlias(s.type, key.first)) {
      continue;
    }
    if (!valueOwn(s.fn, s.value, true, 0)) {
      return false;
    }
  }

  return true;
}

bool Ownership::needsOwn(const Needs &needs) {
  return llvm::all_of(needs, [&](const FieldKey &key) {
    if (notOwn_.count(key) || (!deciding_ && !evaluated_.count(key))) {
      return false;
    }
    return fieldOpen(key);
  });
}

bool Ownership::own(const OwnedRef *owned) {
  return owned_.contains(owned->root) && needsOwn(owned->needs);
}

std::optional<Ref> Ownership::substituted(Ref r, const FnInfo &callee, unsigned caller,
                                          const CallRec &rec, const Callee &call) {
  if (r.kind != RefKind::Owned) {
    return r;
  }
  auto *owned = static_cast<const OwnedRef *>(r.ptr);
  Ref plain{RefKind::Type, owned->type};
  auto found = paramOf_.find(owned->root);
  if (found == paramOf_.end()) {
    return freshOf_.count(owned->root) ? r : plain;
  }
  if (found->second.first != callee.decl || leaking_.contains(owned->root)) {
    return plain;
  }
  int arg = argOf(caller, rec, call, found->second.second, false);
  if (arg < 0) {
    return plain;
  }
  const Expr *value = rec.call->args[size_t(arg)];
  Needs needs;
  if (const VarSymbol *root = rootOf(analysis_.fns_[caller].decl, value, needs, 0)) {
    auto *name = dyn_cast<NameExpr>(strip(value));
    bool whole = owned->whole && needs.empty() && name && name->var == root;
    needs.insert(needs.end(), owned->needs.begin(), owned->needs.end());
    return interned(root, owned->type, std::move(needs), whole);
  }
  if (needsOwn(owned->needs) && valueOwn(caller, value, true, 0)) {
    return std::nullopt;
  }

  return plain;
}

void Ownership::rewrite() {
  auto resolved = [&](Ref r) {
    if (r.kind != RefKind::Owned) {
      return r;
    }
    auto *owned = static_cast<const OwnedRef *>(r.ptr);
    return own(owned) ? Ref{RefKind::Local, nullptr} : owned->fallback;
  };
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
        if (!own(owned)) {
          b.loc.ref = owned->fallback;
          next.insert(b);
          continue;
        }
        bool inner = a.loc.kind == LocKind::Contents &&
                     llvm::any_of(analysis_.aliasAnalysis().inside(owned->type), holdsReferences);
        if (inner) {
          b.loc.ref = Ref{RefKind::Type, owned->type};
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
  if (!fn_) {
    if (collecting_) {
      for (const Stmt *item : analysis_.program_.items) {
        if (!isa<FunctionDecl>(item) && !analysis_.libraryEffects().inLibrary(item->loc())) {
          visitStmt(item);
        }
      }
    }
    return;
  }
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
  size_t count = analysis_.fns_.size();
  analysis_.ownedParams_.clear();
  owned_.clear();
  leaking_.clear();
  paramOf_.clear();
  freshOf_.clear();
  stores_.clear();
  dirty_.clear();
  open_.clear();
  evaluated_.clear();
  notOwn_.clear();
  fresh_.clear();
  sites_.assign(count, {});
  returns_.assign(count, {});
  tail_.assign(count, 0);
  for (unsigned fi = 0; fi < count; ++fi) {
    const FnInfo &f = analysis_.fns_[fi];
    for (unsigned r = 0; r < f.calls.size(); ++r) {
      for (const Callee &callee : f.calls[r].callees) {
        sites_[callee.fn].push_back({fi, r});
      }
    }
  }
  std::vector<unsigned> scanned;
  for (unsigned fi = 0; fi < count; ++fi) {
    const FunctionDecl *decl = analysis_.fns_[fi].decl;
    if (decl && analysis_.isLibrary(decl)) {
      continue;
    }
    scanned.push_back(fi);
    if (!decl) {
      continue;
    }
    for (unsigned i = 0; i < decl->params.size(); ++i) {
      const VarSymbol *v = decl->params[i].sym;
      if (candidate(decl, v)) {
        paramOf_[v] = {decl, i};
      }
    }
    if (!decl->isGenerator && decl->body) {
      fresh_.insert(fi);
    }
  }
  bool first = true;
  bool changed = true;
  while (changed) {
    changed = false;
    collecting_ = first;
    for (unsigned fi : scanned) {
      scan(fi);
      for (const VarSymbol *v : leaked_) {
        changed |= leaking_.insert(v).second;
      }
    }
    first = false;
  }
  collecting_ = false;
  for (const auto &[v, place] : paramOf_) {
    if (!leaking_.contains(v)) {
      owned_.insert(v);
    }
  }
  for (const auto &[v, fi] : freshOf_) {
    if (!leaking_.contains(v)) {
      owned_.insert(v);
    }
  }
  for (unsigned fi : scanned) {
    const FnInfo &f = analysis_.fns_[fi];
    for (const CallRec &rec : f.calls) {
      if (!rec.call || rec.call->builtin != Builtin::None) {
        continue;
      }
      for (const Expr *arg : rec.call->args) {
        Needs needs;
        rootOf(f.decl, arg, needs, 0);
      }
    }
  }
  deciding_ = true;
  changed = true;
  while (changed) {
    changed = false;
    for (const auto &[v, place] : paramOf_) {
      if (owned_.contains(v) &&
          !callersOwned(analysis_.fnIndex_.lookup(place.first), place.second, false)) {
        owned_.erase(v);
        changed = true;
      }
    }
    for (const auto &[v, fi] : freshOf_) {
      if (!owned_.contains(v)) {
        continue;
      }
      const CallExpr *c = programCall(analysis_.declInit_.lookup(v));
      if (c && !callFresh(fi, c)) {
        owned_.erase(v);
        changed = true;
      }
    }
    for (unsigned fi : scanned) {
      if (!fresh_.contains(fi)) {
        continue;
      }
      for (const Expr *value : returns_[fi]) {
        if (!valueOwn(fi, value, false, 0)) {
          fresh_.erase(fi);
          changed = true;
          break;
        }
      }
    }
    std::vector<FieldKey> keys(asked_.begin(), asked_.end());
    for (const FieldKey &key : keys) {
      if (notOwn_.count(key)) {
        continue;
      }
      evaluated_.insert(key);
      if (!fieldOwn(key)) {
        notOwn_.insert(key);
        changed = true;
      }
    }
    for (const FieldKey &key : asked_) {
      changed |= !evaluated_.count(key);
    }
  }
  deciding_ = false;
  for (const auto &[v, place] : paramOf_) {
    if (owned_.contains(v) &&
        callersOwned(analysis_.fnIndex_.lookup(place.first), place.second, true)) {
      analysis_.ownedParams_.insert(v);
    }
  }
}

}
