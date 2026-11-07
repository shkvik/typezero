#include "compiler/parallel/ownership/ownership.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
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

Ownership::Ownership(Analysis &analysis)
    : analysis_(analysis) {}

std::optional<Ref> Ownership::ownedRef(const Expr *e) {
  const FunctionDecl *fn =
      analysis_.fn && analysis_.fn->decl != analysis_.module ? analysis_.fn->decl : nullptr;
  Needs needs;
  const VarSymbol *root = rootOf(fn, e, needs, 0);
  if (!root || !e->type) {
    return std::nullopt;
  }
  auto *name = dyn_cast<NameExpr>(strip(e));
  bool whole = needs.empty() && name && name->var() == root;

  return interned(root, e->type->nonOptional(), std::move(needs), whole);
}

Ref Ownership::interned(const VarSymbol *root, const Type *type, Needs needs, bool whole) {
  llvm::sort(needs);
  needs.erase(std::unique(needs.begin(), needs.end()), needs.end());
  auto key = std::make_tuple(root, type, needs, whole);
  auto found = analysis_.ownedRefIndex.find(key);
  if (found != analysis_.ownedRefIndex.end()) {
    return Ref{RefKind::Owned, found->second};
  }
  OwnedRef &made = analysis_.ownedRefs.emplace_back();
  made.root = root;
  made.type = type;
  made.needs = needs;
  made.whole = whole;
  made.fallback = Ref{RefKind::Type, type};
  if (whole) {
    auto init = analysis_.declInit.find(root);
    if (init != analysis_.declInit.end() && analysis_.aliasAnalysis.isAllocation(init->second)) {
      made.fallback = Ref{RefKind::Site, init->second};
    }
  }
  analysis_.ownedRefIndex[key] = &made;

  return Ref{RefKind::Owned, &made};
}

bool Ownership::isParam(const FunctionDecl *fn, const VarSymbol *v) {
  return fn && v &&
         (isSelf(fn, v) || llvm::any_of(fn->params, [&](const Param &p) { return p.sym == v; }));
}

bool Ownership::isSelf(const FunctionDecl *fn, const VarSymbol *v) {
  return fn && v && fn->thisVar() == v && fn->takesThis();
}

bool Ownership::builds(const FunctionDecl *fn) {
  return fn && fn->cls() && (fn->isConstructor() || fn == fn->cls()->init);
}

bool Ownership::candidate(const FunctionDecl *fn, const VarSymbol *v) const {
  if (!fn || !v || fn->isGenerator || analysis_.isLibrary(fn)) {
    return false;
  }
  if (isSelf(fn, v)) {
    return fn->body != nullptr;
  }
  for (const Param &p : fn->params) {
    if (p.sym == v) {
      bool callable = v->type && v->type->nonOptional()->isFunction();
      return !p.rest() && !p.defaultValue() && !p.property && !v->reassigned &&
             (holdsReferences(v->type) || callable);
    }
  }

  return false;
}

bool Ownership::tracked(const FunctionDecl *fn, const VarSymbol *v) {
  return fn && v && v->owner() == fn && !v->isGlobal() && !v->reassigned;
}

const FunctionDecl *Ownership::homeOf(const FunctionDecl *fn, const VarSymbol *v) {
  if (!fn || !v || !v->captured() || v->isGlobal() || !v->owner() || v->owner() == fn) {
    return nullptr;
  }
  for (const FunctionDecl *up = fn->parent; up; up = up->parent) {
    if (up == v->owner()) {
      return up;
    }
  }

  return nullptr;
}

const std::vector<VarSymbol *> &Ownership::capturesOf(const FunctionDecl *fn) {
  return fn->group ? fn->group->captures : fn->captures;
}

const FunctionDecl *Ownership::closureOf(const Expr *e) {
  auto *fe = dyn_cast_or_null<FunctionExpr>(strip(e));

  return fe ? fe->fn : nullptr;
}

void Ownership::closure(const FunctionDecl *fn, bool confined) {
  if (confined && !fn->isAsync && !fn->isGenerator) {
    goodClosures_.insert(fn);
    return;
  }
  badClosures_.insert(fn);
  for (const VarSymbol *v : capturesOf(fn)) {
    Roots roots;
    carriersOf(v, roots, 0);
    for (const VarSymbol *root : roots) {
      leaked_.insert(root);
    }
  }
}

bool Ownership::confined(const FunctionDecl *fn) const {
  return goodClosures_.contains(fn) && !badClosures_.contains(fn);
}

bool Ownership::confinedVar(const VarSymbol *v) const {
  if (!v || v->isGlobal() || !v->captured() || !v->owner()) {
    return false;
  }
  auto found = capturers_.find(v);
  if (found == capturers_.end()) {
    return false;
  }

  return llvm::all_of(found->second, [&](const FunctionDecl *fn) { return confined(fn); });
}

bool Ownership::freshCandidate(const FunctionDecl *fn, const VarSymbol *v) const {
  if (!fn || fn->isGenerator || analysis_.isLibrary(fn) || !tracked(fn, v) ||
      !holdsReferences(v->type)) {
    return false;
  }
  auto init = analysis_.declInit.find(v);
  if (init == analysis_.declInit.end()) {
    return false;
  }

  return freshAllocation(init->second) || programCall(init->second);
}

bool Ownership::freshAllocation(const Expr *e) const {
  e = strip(e);
  if (!e || !analysis_.aliasAnalysis.isAllocation(e)) {
    return false;
  }
  auto *n = dyn_cast<NewExpr>(e);

  return !(n && n->cls() && analysis_.escapeScan.constructorLeaks(n->cls()));
}

const CallExpr *Ownership::programCall(const Expr *e) const {
  e = strip(e);
  if (auto *a = dyn_cast_or_null<AwaitExpr>(e)) {
    e = strip(a->value);
  }
  auto *c = dyn_cast_or_null<CallExpr>(e);
  if (!c || c->builtin() != Builtin::None || analysis_.libraryEffects.libraryTarget(c)) {
    return nullptr;
  }

  return c;
}

bool Ownership::libraryField(const Type *object, llvm::StringRef name) {
  for (const ClassDecl *k = object->classDecl(); k; k = k->parent) {
    if (!k->library()) {
      continue;
    }
    for (const ClassField &f : k->fields) {
      if (f.name == name && !f.isStatic()) {
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
  if (type && type->classDecl() && !type->classDecl()->library()) {
    return true;
  }
  if (!containers_) {
    containers_.emplace();
    std::set<const Type *> library;
    for (const Stmt *item : analysis_.program.items) {
      if (analysis_.libraryEffects.inLibrary(item->loc())) {
        libraryTypes(item, library);
        continue;
      }
      auto *alias = dyn_cast<TypeAliasDecl>(item);
      if (alias && alias->resolved && alias->typeParams.empty() &&
          plainObject(alias->resolved->nonOptional())) {
        containers_->insert(alias->resolved->nonOptional());
      }
    }
    std::vector<const Type *> candidates(containers_->begin(), containers_->end());
    for (const Type *t : candidates) {
      for (const Type *other : library) {
        if (other->kind() == Type::Kind::Object && !other->isAnyObject() &&
            analysis_.aliasAnalysis.objectTypesMayAlias(other, t)) {
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
        for (const auto &[from, to] : analysis_.program.views) {
          const Type *other = from == t ? to : to == t ? from : nullptr;
          foreign |= other && !containers_->contains(other);
        }
        for (const Type *sub : analysis_.program.subtypes) {
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
    if (cls->generic()) {
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

    return (cls && cls->library()) || type->isCrypto();
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
      return !p.spread() && !p.method() && valueArgument(p.value);
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
    const VarSymbol *v = n->var();
    if (const FunctionDecl *home = homeOf(fn, v)) {
      return rootOf(home, e, needs, depth + 1);
    }
    if (isParam(fn, v)) {
      return candidate(fn, v) ? v : nullptr;
    }
    if (!tracked(fn, v)) {
      return nullptr;
    }
    if (freshCandidate(fn, v)) {
      return v;
    }
    auto found = analysis_.declInit.find(v);
    if (found == analysis_.declInit.end()) {
      return nullptr;
    }

    return rootOf(fn, found->second, needs, depth + 1);
  }
  if (auto *m = dyn_cast<MemberExpr>(e)) {
    if (m->method() || !m->object || !m->object->type || !m->type) {
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
    if (const Expr *same = cryptoSelf(c)) {
      return rootOf(fn, same, needs, depth + 1);
    }
    const FunctionDecl *lib = analysis_.libraryEffects.libraryTarget(c);
    auto *m = dyn_cast<MemberExpr>(c->callee);
    if (!lib || !lib->cls() || lib->isStatic() || lib->isAsync || lib->isGenerator ||
        !c->method() || !m || c->hasSpread()) {
      return nullptr;
    }
    if (!llvm::all_of(c->args, valueArgument) || !derivable(c->type, 0)) {
      return nullptr;
    }

    return rootOf(fn, m->object, needs, depth + 1);
  }

  return nullptr;
}

const Expr *Ownership::cryptoSelf(const CallExpr *c) {
  if (c->builtin() != Builtin::Crypto || c->args.empty()) {
    return nullptr;
  }
  auto *number = dyn_cast<IntLitExpr>(strip(c->args[0]));
  if (!number || number->value >= std::size(kCryptoFunctions) ||
      kCryptoFunctions[size_t(number->value)].result != 'S') {
    return nullptr;
  }
  llvm::StringRef kinds = kCryptoFunctions[size_t(number->value)].params;
  for (size_t i = 1; i < c->args.size() && i - 1 < kinds.size(); ++i) {
    if (kinds[i - 1] == 'o') {
      return c->args[i];
    }
  }

  return nullptr;
}

void Ownership::carriers(const Expr *e, Roots &out, unsigned depth) {
  e = strip(e);
  if (!e || depth > 64 || !holdsReferences(e->type)) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::Name:
    carriersOf(cast<NameExpr>(e)->var(), out, depth);
    return;
  case Expr::Kind::Member:
    carriers(cast<MemberExpr>(e)->object, out, depth + 1);
    return;
  case Expr::Kind::Index:
    carriers(cast<IndexExpr>(e)->object, out, depth + 1);
    return;
  default:
    carriersOther(e, out, depth);
    return;
  }
}

void Ownership::carriersOf(const VarSymbol *v, Roots &out, unsigned depth) {
  if (!v || depth > 64) {
    return;
  }
  if (const FunctionDecl *home = homeOf(fn_, v)) {
    const FunctionDecl *saved = fn_;
    fn_ = home;
    carriersOf(v, out, depth + 1);
    fn_ = saved;
    return;
  }
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
  if (auto init = analysis_.declInit.find(v); init != analysis_.declInit.end()) {
    carriers(init->second, out, depth + 1);
  }
}

void Ownership::carriersOther(const Expr *e, Roots &out, unsigned depth) {
  switch (e->kind()) {
  case Expr::Kind::Call: {
    auto *c = cast<CallExpr>(e);
    switch (c->builtin()) {
      case Builtin::BufferConcat:
      case Builtin::BytesFromArray:
      case Builtin::BytesFromBytes:
      case Builtin::BufferFromString:
      case Builtin::TextEncode:
        return;
      case Builtin::Crypto: {
        auto *number = c->args.empty() ? nullptr : dyn_cast<IntLitExpr>(strip(c->args[0]));
        if (!number || number->value >= std::size(kCryptoFunctions)) {
          break;
        }
        const CryptoFunction &fn = kCryptoFunctions[size_t(number->value)];
        if (fn.result != 'S') {
          return;
        }
        llvm::StringRef kinds = fn.params;
        for (size_t i = 1; i < c->args.size() && i - 1 < kinds.size(); ++i) {
          if (kinds[i - 1] == 'o') {
            carriers(c->args[i], out, depth + 1);
          }
        }
        return;
      }
      default:
        break;
    }
    if (analysis_.libraryEffects.libraryTarget(c) &&
        (plainValues(c->type, 0) || analysis_.aliasAnalysis.isAllocation(c))) {
      return;
    }
    bool program = c->builtin() == Builtin::None && !analysis_.libraryEffects.libraryTarget(c);
    if (auto *m = dyn_cast<MemberExpr>(c->callee);
        m && (!program || !passesOwnedSelf(c) || returnsBack(c, kSelf))) {
      carriers(m->object, out, depth + 1);
    }
    for (size_t i = 0; i < c->args.size(); ++i) {
      if (!program || !passesOwned(c, i) || returnsBack(c, i)) {
        carriers(c->args[i], out, depth + 1);
      }
    }
    return;
  }
  case Expr::Kind::New: {
    auto *n = cast<NewExpr>(e);
    for (const Expr *arg : n->ctor() ? n->ctor()->args : n->args) {
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
      if (!p.method()) {
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
    if (!analysis_.aliasAnalysis.isAllocation(e)) {
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
    if (returningIn_ == fn_ && !v->captured() && freshCandidate(fn_, v)) {
      continue;
    }
    leaked_.insert(v);
  }
}

void Ownership::returnParams(const Expr *e) {
  Roots roots;
  carriers(e, roots, 0);
  for (const VarSymbol *v : roots) {
    if (isParam(fn_, v)) {
      returned_.insert(v);
    }
  }
}

bool Ownership::returnsBack(const CallExpr *c, size_t index) {
  const CallRec *rec = recOf(c);
  if (!rec) {
    return true;
  }
  size_t shift = thisFirst(*rec) ? 1 : 0;
  for (int edgeIndex : rec->edges) {
    const CallEdge &callee = analysis_.callGraph.edge(edgeIndex);
    const FunctionDecl *g = analysis_.byFunction[callee.to]->decl;
    if (!g) {
      return true;
    }
    const VarSymbol *v = nullptr;
    if (index == kSelf) {
      v = g->thisVar();
    } else if (index >= shift && index - shift < g->params.size()) {
      v = g->params[index - shift].sym;
    }
    if (!v || returned_.contains(v)) {
      return true;
    }
  }

  return false;
}

void Ownership::literal(const ObjectLitExpr *o) {
  const Type *type = o->type ? o->type->nonOptional() : nullptr;
  if (!type) {
    return;
  }
  for (const ObjectLitExpr::Property &p : o->props) {
    if (p.method()) {
      continue;
    }
    if (p.spread() || p.key()) {
      dirty_[analysis_.star].push_back(type);
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
  std::vector<const Type *> &all = dirty_[analysis_.star];
  all.push_back(type);
  for (const Type *inner : analysis_.aliasAnalysis.inside(type)) {
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
    const FunctionDecl *outer = std::exchange(returningIn_, fn_);
    visit(value);
    returningIn_ = outer;
    returnParams(value);
    if (collecting_ && value) {
      returns_[fi_].push_back(value);
    }
    return;
  }
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(s);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic() && f.init) {
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
  case Expr::Kind::Function:
    closure(cast<FunctionExpr>(e)->fn, false);
    return;
  case Expr::Kind::Name:
    if (auto *n = cast<NameExpr>(e); n->func() && n->func()->decl && n->func()->decl->parent) {
      closure(n->func()->decl, false);
    }
    return;
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
      visit(p.key());
      if (!p.method()) {
        visit(p.value);
      }
    }
    return;
  }
  case Expr::Kind::Member: {
    auto *m = cast<MemberExpr>(e);
    visit(m->object);
    if (m->method()) {
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
    auto *assigned = dyn_cast<AssignExpr>(e);
    bool selfField = false;
    if (auto *m = dyn_cast<MemberExpr>(target)) {
      auto *self = dyn_cast<NameExpr>(strip(m->object));
      selfField = assigned && !assigned->op && self && isSelf(fn_, self->var()) &&
                  m->object->type && m->object->type->nonOptional()->classDecl();
      if (collecting_ && selfField) {
        const ClassDecl *built = builds(fn_) ? fn_->cls() : nullptr;
        stores_[analysis_.intern(m->name)].push_back(
            {m->object->type->nonOptional(), assigned->value, fi_, nullptr, built});
      } else if (collecting_) {
        dirty(m->object, m->name);
      }
      visit(m->object);
    } else if (auto *ix = dyn_cast<IndexExpr>(target)) {
      if (collecting_ && !ix->field().empty()) {
        dirty(ix->object, ix->field());
      } else if (collecting_ && ix->keyed()) {
        dirty(ix->object, "*");
      }
      visit(ix->object);
      visit(ix->index);
    } else if (!isa<NameExpr>(target)) {
      visit(target);
    }
    if (assigned) {
      visit(assigned->value);
      bool kept = selfField && builds(fn_) && !leaking_.contains(fn_->thisVar()) &&
                  !analysis_.escapeScan.constructorLeaks(fn_->cls());
      if (selfField && !builds(fn_) && candidate(fn_, fn_->thisVar())) {
        kept = true;
        if (collecting_) {
          selfStores_.push_back({fn_->thisVar(), assigned->value, fi_, {}});
        }
      }
      if (!kept) {
        leak(assigned->value);
      }
    }
    return;
  }
  case Expr::Kind::Call:
    visitCall(cast<CallExpr>(e));
    return;
  case Expr::Kind::New: {
    auto *n = cast<NewExpr>(e);
    const std::vector<Expr *> &args = n->ctor() ? n->ctor()->args : n->args;
    for (size_t i = 0; i < args.size(); ++i) {
      visit(args[i]);
      if (!passesOwnedNew(n, i)) {
        leak(args[i]);
      }
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
  if (c->builtin() != Builtin::None) {
    if (collecting_ && !knownBuiltin(c->builtin())) {
      dirtyAll(m ? m->object : nullptr);
      for (const Expr *arg : c->args) {
        dirtyAll(arg);
      }
    }
    if (m) {
      visit(m->object);
    }
    if (c->builtin() == Builtin::ReflectApply || c->builtin() == Builtin::ReflectConstruct ||
        c->builtin() == Builtin::ReflectFill) {
      bool apply = c->builtin() == Builtin::ReflectApply;
      bool keepsSelf = apply && reflectKeepsArgs(c, true);
      bool keepsArgs = reflectKeepsArgs(c, false);
      for (size_t i = 0; i < c->args.size(); ++i) {
        visit(c->args[i]);
        bool kept = apply ? (i == 1 && keepsSelf) || (i == 2 && keepsArgs) : i >= 1 && keepsArgs;
        if (!kept) {
          leak(c->args[i]);
        }
      }
      return;
    }
    bool reads = onlyReadsArgs(c->builtin()) || c->builtin() == Builtin::ResultOk ||
                 c->builtin() == Builtin::ResultErr || c->builtin() == Builtin::Crypto;
    Needs held;
    const VarSymbol *holder = nullptr;
    if (m && (c->builtin() == Builtin::ArrPush || c->builtin() == Builtin::ArrUnshift ||
              c->builtin() == Builtin::MapSet || c->builtin() == Builtin::SetAdd)) {
      holder = rootOf(fn_, m->object, held, 0);
    }
    if (holder) {
      for (const Expr *arg : c->args) {
        if (const FunctionDecl *fn = closureOf(arg)) {
          closure(fn, false);
          continue;
        }
        visit(arg);
        if (collecting_) {
          selfStores_.push_back({holder, arg, fi_, held});
        }
      }
      return;
    }
    for (const Expr *arg : c->args) {
      if (const FunctionDecl *fn = closureOf(arg)) {
        closure(fn, fn->nonEscaping);
        continue;
      }
      visit(arg);
      if (!reads) {
        leak(arg);
      }
    }
    return;
  }
  if (analysis_.libraryEffects.libraryTarget(c)) {
    if (m && c->method()) {
      visit(m->object);
    }
    for (const Expr *arg : c->args) {
      if (const FunctionDecl *fn = closureOf(arg)) {
        closure(fn, false);
        continue;
      }
      visit(arg);
      leak(arg);
    }
    return;
  }
  if (m && (c->method() || !c->unionMethods().empty())) {
    visit(m->object);
    if (!passesOwnedSelf(c)) {
      leak(m->object);
    }
  } else if (!c->func()) {
    visit(c->callee);
  }
  for (size_t i = 0; i < c->args.size(); ++i) {
    if (const FunctionDecl *fn = closureOf(c->args[i])) {
      closure(fn, passesOwned(c, i) && !returnsBack(c, i));
      continue;
    }
    visit(c->args[i]);
    if (!passesOwned(c, i)) {
      leak(c->args[i]);
    }
  }
}

bool Ownership::thisFirst(const CallRec &rec) const {
  const CallExpr *c = rec.call;
  if (!c || c->method() || c->func() || !c->unionMethods().empty() || !c->callee ||
      !c->callee->type) {
    return false;
  }
  const Type *type = c->callee->type->nonOptional();

  return type->isFunction() && type->hasThis();
}

bool Ownership::tailAwaited(int fi) {
  if (tail_[fi] == 1) {
    return false;
  }
  if (tail_[fi] > 1) {
    return tail_[fi] == 2;
  }
  const FnInfo &f = *analysis_.byFunction[fi];
  if (f.decl == analysis_.module || f.async || f.asyncGen || f.syncGen) {
    tail_[fi] = 3;
    return false;
  }
  tail_[fi] = 1;
  const std::vector<int> &incoming = analysis_.callGraph.incoming(fi);
  bool awaited = !incoming.empty();
  for (int edgeIndex : incoming) {
    const CallEdge &call = analysis_.callGraph.edge(edgeIndex);
    const FnInfo &caller = *analysis_.byFunction[call.from];
    const CallRec &rec = caller.calls[call.call];
    if (rec.ownArgs > 0) {
      continue;
    }
    bool direct = rec.kind == CallKind::Call && rec.reflect == CallRec::Reflect::None &&
                  rec.channel < 0 && rec.call && rec.call->builtin() == Builtin::None;
    bool waits =
        rec.await || (rec.returned && caller.async) || (rec.tail && tailAwaited(call.from));
    if (!direct || !waits) {
      awaited = false;
      break;
    }
  }
  tail_[fi] = awaited ? 2 : 3;

  return awaited;
}

bool Ownership::handsOver(int fi, const CallRec &rec, Mode mode, bool strict) {
  if (mode == Mode::Sync || mode == Mode::Await) {
    return true;
  }

  return mode == Mode::Spawn && !strict && rec.tail && tailAwaited(fi);
}

const std::vector<Expr *> *Ownership::argsOf(const CallRec &rec) {
  if (rec.call) {
    return rec.call->hasSpread() ? nullptr : &rec.call->args;
  }
  auto *n = dyn_cast_or_null<NewExpr>(static_cast<const Expr *>(rec.site));
  if (!n || !n->cls() || n->cls()->library()) {
    return nullptr;
  }
  if (n->ctor()) {
    return n->ctor()->hasSpread() ? nullptr : &n->ctor()->args;
  }

  return &n->args;
}

int Ownership::argOf(int fi, const CallRec &rec, const CallEdge &callee, size_t param,
                     bool strict) {
  const std::vector<Expr *> *args = argsOf(rec);
  if (rec.ownArgs > 0 || rec.kind != CallKind::Call || rec.reflect != CallRec::Reflect::None ||
      rec.channel >= 0 || !args || (rec.call && rec.call->builtin() != Builtin::None) ||
      !handsOver(fi, rec, callee.mode, strict)) {
    return -1;
  }
  size_t index = param + (thisFirst(rec) ? 1 : 0);

  return index < args->size() ? int(index) : -1;
}

const CallRec *Ownership::recOf(const void *site) const {
  const FnInfo &f = *analysis_.byFunction[fi_];
  auto found = f.callOf.find(std::make_pair(site, unsigned(CallKind::Call)));
  if (found == f.callOf.end()) {
    return nullptr;
  }
  const CallRec &rec = f.calls[found->second];
  if (rec.reflect != CallRec::Reflect::None || rec.channel >= 0 || rec.edges.empty()) {
    return nullptr;
  }

  return &rec;
}

bool Ownership::passesOwnedSelf(const CallExpr *c) {
  const CallRec *rec = c->method() && c->unionMethods().empty() ? recOf(c) : nullptr;
  if (!rec) {
    return false;
  }
  for (int edgeIndex : rec->edges) {
    const CallEdge &callee = analysis_.callGraph.edge(edgeIndex);
    const FunctionDecl *g = analysis_.byFunction[callee.to]->decl;
    if (!handsOver(fi_, *rec, callee.mode, false) || !g || !candidate(g, g->thisVar()) ||
        leaking_.contains(g->thisVar())) {
      return false;
    }
  }

  return true;
}

bool Ownership::passesOwnedNew(const NewExpr *n, size_t index) {
  const CallRec *rec = recOf(n);
  const std::vector<Expr *> *args = rec ? argsOf(*rec) : nullptr;
  if (!args || index >= args->size()) {
    return false;
  }
  for (int edgeIndex : rec->edges) {
    const CallEdge &callee = analysis_.callGraph.edge(edgeIndex);
    const FunctionDecl *g = analysis_.byFunction[callee.to]->decl;
    if (!g || !g->isConstructor()) {
      continue;
    }
    if (!handsOver(fi_, *rec, callee.mode, false) || index >= g->params.size()) {
      return false;
    }
    const VarSymbol *param = g->params[index].sym;
    if (!candidate(g, param) || leaking_.contains(param)) {
      return false;
    }
  }

  return true;
}

bool Ownership::passesOwned(const CallExpr *c, size_t index) {
  if (c->hasSpread()) {
    return false;
  }
  const CallRec *found = recOf(c);
  if (!found) {
    const FnInfo &f = *analysis_.byFunction[fi_];
    auto raw =
        f.callOf.find(std::make_pair(static_cast<const void *>(c), unsigned(CallKind::Call)));
    if (raw != f.callOf.end()) {
      const CallRec &rec = f.calls[raw->second];
      if (rec.reflect == CallRec::Reflect::None && rec.channel < 0 && rec.edges.empty() &&
          (rec.byType || !rec.byName.empty())) {
        return true;
      }
    }
    return false;
  }
  const CallRec &rec = *found;
  size_t shift = thisFirst(rec) ? 1 : 0;
  if (index < shift) {
    return false;
  }
  for (int edgeIndex : rec.edges) {
    const CallEdge &callee = analysis_.callGraph.edge(edgeIndex);
    const FunctionDecl *g = analysis_.byFunction[callee.to]->decl;
    if (!handsOver(fi_, rec, callee.mode, false)) {
      return false;
    }
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

bool Ownership::reflectKeepsArgs(const CallExpr *c, bool self) {
  const CallRec *found = recOf(c);
  if (!found) {
    return false;
  }
  for (int edgeIndex : found->edges) {
    const CallEdge &callee = analysis_.callGraph.edge(edgeIndex);
    const FunctionDecl *g = analysis_.byFunction[callee.to]->decl;
    if (!g) {
      return false;
    }
    if (self) {
      if (!candidate(g, g->thisVar()) || leaking_.contains(g->thisVar())) {
        return false;
      }
      continue;
    }
    for (const Param &p : g->params) {
      const Type *type = p.sym ? p.sym->type : nullptr;
      if (type && !holdsReferences(type) && !type->nonOptional()->isFunction()) {
        continue;
      }
      if (!candidate(g, p.sym) || leaking_.contains(p.sym)) {
        return false;
      }
    }
  }

  return true;
}

bool Ownership::callersOwned(int fi, size_t index, bool strict) {
  for (int edgeIndex : analysis_.callGraph.incoming(fi)) {
    const CallEdge &callee = analysis_.callGraph.edge(edgeIndex);
    const CallRec &rec = analysis_.byFunction[callee.from]->calls[callee.call];
    if (rec.ownArgs > 0) {
      if (index < rec.ownArgs && (!rec.poolItem || poolFresh(rec.poolItem))) {
        continue;
      }
      return false;
    }
    if (index == kSelf) {
      if (!selfOwned(callee.from, rec, callee, strict)) {
        return false;
      }
      continue;
    }
    int arg = argOf(callee.from, rec, callee, index, strict);
    if (arg < 0 || !valueOwn(callee.from, (*argsOf(rec))[size_t(arg)], true, 0)) {
      return false;
    }
  }

  return true;
}

const Expr *Ownership::receiverOf(const CallRec &rec) {
  const CallExpr *c = rec.call;
  if (!c || !c->method() || c->superCall() || !c->unionMethods().empty() || rec.ownArgs > 0 ||
      rec.kind != CallKind::Call || rec.reflect != CallRec::Reflect::None || rec.channel >= 0) {
    return nullptr;
  }
  auto *m = dyn_cast<MemberExpr>(c->callee);

  return m ? m->object : nullptr;
}

bool Ownership::selfOwned(int fi, const CallRec &rec, const CallEdge &callee, bool strict) {
  const FunctionDecl *g = analysis_.byFunction[callee.to]->decl;
  if (!handsOver(fi, rec, callee.mode, strict)) {
    return false;
  }
  if (builds(g) && llvm::isa_and_nonnull<NewExpr>(static_cast<const Expr *>(rec.site))) {
    return true;
  }
  const FunctionDecl *caller = analysis_.byFunction[fi]->decl;
  if (rec.call && rec.call->superCall() && builds(g) && caller && caller->thisVar()) {
    return owned_.contains(caller->thisVar());
  }
  const Expr *receiver = receiverOf(rec);

  return receiver && valueOwn(fi, receiver, true, 0);
}

bool Ownership::poolFresh(const Type *item) const {
  if (analysis_.poolOpen) {
    return false;
  }
  for (const auto &[type, create] : analysis_.poolCreates) {
    if (!analysis_.aliasAnalysis.typesMayAlias(type, item)) {
      continue;
    }
    if (analysis_.byFunction[create->id] == nullptr || !fresh_.contains(create->id)) {
      return false;
    }
  }

  return true;
}

bool Ownership::callFresh(int fi, const CallExpr *c) const {
  const FnInfo &f = *analysis_.byFunction[fi];
  auto found =
      f.callOf.find(std::make_pair(static_cast<const void *>(c), unsigned(CallKind::Call)));
  if (found == f.callOf.end()) {
    return false;
  }
  const CallRec &rec = f.calls[found->second];
  if (rec.reflect != CallRec::Reflect::None || rec.channel >= 0 || rec.edges.empty()) {
    return false;
  }

  return llvm::all_of(rec.edges, [&](int edgeIndex) {
    const CallEdge &callee = analysis_.callGraph.edge(edgeIndex);
    return (callee.mode == Mode::Sync || callee.mode == Mode::Await) && fresh_.contains(callee.to);
  });
}

bool Ownership::valueOwn(int fi, const Expr *e, bool params, unsigned depth) {
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
  if (auto *c = dyn_cast<CallExpr>(e);
      c && (c->builtin() == Builtin::ResultOk || c->builtin() == Builtin::ResultErr)) {
    return c->args.empty() || valueOwn(fi, c->args[0], params, depth + 1);
  }
  const FunctionDecl *fn = analysis_.byFunction[fi]->decl;
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
  for (const char *field : {key.second, analysis_.star}) {
    auto types = dirty_.find(field);
    if (types == dirty_.end()) {
      continue;
    }
    for (const Type *type : types->second) {
      if (analysis_.aliasAnalysis.typesMayAlias(type, key.first)) {
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
        (s.built && analysis_.escapeScan.constructorLeaks(s.built)) ||
        !analysis_.aliasAnalysis.typesMayAlias(s.type, key.first)) {
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

std::optional<Ref> Ownership::substituted(Ref r, const FnInfo &callee, int caller,
                                          const CallRec &rec, const CallEdge &call) {
  if (r.kind != RefKind::Owned) {
    return r;
  }
  auto *owned = static_cast<const OwnedRef *>(r.ptr);
  Ref plain{RefKind::Type, owned->type};
  auto found = paramOf_.find(owned->root);
  if (found == paramOf_.end()) {
    return freshOf_.count(owned->root) ? r : plain;
  }
  if (leaking_.contains(owned->root)) {
    return plain;
  }
  if (found->second.first != callee.decl) {
    return owned->root->captured() ? r : plain;
  }
  const Expr *value = nullptr;
  if (found->second.second == kSelf) {
    if (!handsOver(caller, rec, call.mode, false)) {
      return plain;
    }
    if (builds(callee.decl) &&
        llvm::isa_and_nonnull<NewExpr>(static_cast<const Expr *>(rec.site))) {
      bool fresh = !analysis_.escapeScan.constructorLeaks(callee.decl->cls());
      return fresh && needsOwn(owned->needs) ? std::nullopt : std::optional<Ref>(plain);
    }
    const FunctionDecl *from = analysis_.byFunction[caller]->decl;
    if (rec.call && rec.call->superCall() && builds(callee.decl) && from &&
        candidate(from, from->thisVar())) {
      return interned(from->thisVar(), owned->type, owned->needs, owned->whole);
    }
    value = receiverOf(rec);
  } else {
    int arg = argOf(caller, rec, call, found->second.second, false);
    if (arg >= 0) {
      value = (*argsOf(rec))[size_t(arg)];
    }
  }
  if (!value) {
    return plain;
  }
  Needs needs;
  if (const VarSymbol *root = rootOf(analysis_.byFunction[caller]->decl, value, needs, 0)) {
    auto *name = dyn_cast<NameExpr>(strip(value));
    bool whole = owned->whole && needs.empty() && name && name->var() == root;
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
  capturers_.clear();
  for (const FnInfo &f : analysis_.records) {
    if (f.decl != analysis_.module && !analysis_.isLibrary(f.decl)) {
      for (const VarSymbol *v : capturesOf(f.decl)) {
        capturers_[v].push_back(f.decl);
      }
    }
  }
  auto taskVar = [&](const Access &a) {
    return a.loc.kind == LocKind::Var && confinedVar(static_cast<const VarSymbol *>(a.loc.key));
  };
  for (CallEdge &edge : analysis_.callGraph) {
    if (edge.self) {
      edge.self = resolved(*edge.self);
    }
  }
  for (FnInfo &f : analysis_.records) {
    for (Segment &seg : f.segs) {
      bool any = llvm::any_of(seg.effects, [&](const Access &a) {
        return a.loc.ref.kind == RefKind::Owned || taskVar(a);
      });
      if (!any) {
        continue;
      }
      Effects next;
      for (const Access &a : seg.effects) {
        if (taskVar(a)) {
          continue;
        }
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
                     llvm::any_of(analysis_.aliasAnalysis.inside(owned->type), holdsReferences);
        if (inner) {
          b.loc.ref = Ref{RefKind::Type, owned->type};
          b.loc.key = analysis_.inner;
          next.insert(b);
        }
      }
      seg.effects = std::move(next);
    }
    for (CallRec &rec : f.calls) {
      if (rec.bySelf) {
        rec.bySelf = resolved(*rec.bySelf);
      }
    }
  }
}

void Ownership::loops(const FunctionDecl *fn, const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *x : cast<BlockStmt>(s)->body) {
      loops(fn, x);
    }
    return;
  case Stmt::Kind::If:
    loops(fn, cast<IfStmt>(s)->thenStmt);
    loops(fn, cast<IfStmt>(s)->elseStmt);
    return;
  case Stmt::Kind::While:
    loops(fn, cast<WhileStmt>(s)->body);
    return;
  case Stmt::Kind::DoWhile:
    loops(fn, cast<DoWhileStmt>(s)->body);
    return;
  case Stmt::Kind::For:
    loops(fn, cast<ForStmt>(s)->init);
    loops(fn, cast<ForStmt>(s)->body);
    return;
  case Stmt::Kind::ForOf: {
    auto *f = cast<ForOfStmt>(s);
    if (tracked(fn, f->binding.sym)) {
      loopInit_[f->binding.sym] = f->iterable;
    }
    loops(fn, f->body);
    return;
  }
  case Stmt::Kind::Switch:
    for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
      for (const Stmt *x : c.body) {
        loops(fn, x);
      }
    }
    return;
  case Stmt::Kind::Labeled:
    loops(fn, cast<LabeledStmt>(s)->body);
    return;
  default:
    return;
  }
}

void Ownership::scan(int fi) {
  fi_ = fi;
  const FunctionDecl *decl = analysis_.byFunction[fi]->decl;
  fn_ = decl == analysis_.module ? nullptr : decl;
  leaked_.clear();
  if (!fn_) {
    if (collecting_) {
      for (const Stmt *item : analysis_.program.items) {
        if (!isa<FunctionDecl>(item) && !analysis_.libraryEffects.inLibrary(item->loc())) {
          visitStmt(item);
        }
      }
    }
    return;
  }
  for (const Param &p : fn_->params) {
    if (p.defaultValue()) {
      visit(p.defaultValue());
      leak(p.defaultValue());
    }
  }
  visitStmt(fn_->body);
  fn_ = nullptr;
}

void Ownership::decide() {
  forced_.clear();
  for (int round = 0; round < 32; ++round) {
    decideOnce();
    size_t before = forced_.size();
    deciding_ = true;
    for (const SelfStore &store : selfStores_) {
      if (owned_.contains(store.self) && needsOwn(store.needs)) {
        continue;
      }
      fi_ = store.fi;
      fn_ = analysis_.byFunction[store.fi]->decl;
      Roots roots;
      carriers(store.value, roots, 0);
      llvm::erase(roots, store.self);
      forced_.insert(roots.begin(), roots.end());
    }
    fn_ = nullptr;
    deciding_ = false;
    if (forced_.size() == before) {
      return;
    }
  }
}

void Ownership::decideOnce() {
  size_t count = analysis_.byFunction.size();
  analysis_.ownedParams.clear();
  owned_.clear();
  leaking_.clear();
  leaking_.insert(forced_.begin(), forced_.end());
  selfStores_.clear();
  returned_.clear();
  goodClosures_.clear();
  badClosures_.clear();
  loopInit_.clear();
  paramOf_.clear();
  freshOf_.clear();
  stores_.clear();
  dirty_.clear();
  open_.clear();
  evaluated_.clear();
  notOwn_.clear();
  fresh_.clear();
  returns_.assign(count, {});
  tail_.assign(count, 0);
  std::vector<int> scanned;
  for (const FnInfo &f : analysis_.records) {
    const FunctionDecl *decl = f.decl;
    if (decl != analysis_.module && analysis_.isLibrary(decl)) {
      continue;
    }
    scanned.push_back(decl->id);
    if (decl == analysis_.module) {
      continue;
    }
    for (unsigned i = 0; i < decl->params.size(); ++i) {
      const VarSymbol *v = decl->params[i].sym;
      if (candidate(decl, v)) {
        paramOf_[v] = {decl, i};
      }
    }
    if (candidate(decl, decl->thisVar())) {
      paramOf_[decl->thisVar()] = {decl, kSelf};
    }
    if (!decl->isGenerator && decl->body) {
      fresh_.insert(decl->id);
    }
    loops(decl, decl->body);
  }
  bool first = true;
  bool changed = true;
  while (changed) {
    changed = false;
    collecting_ = first;
    for (int fi : scanned) {
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
  for (int fi : scanned) {
    const FnInfo &f = *analysis_.byFunction[fi];
    for (const CallRec &rec : f.calls) {
      if (!rec.call || rec.call->builtin() != Builtin::None) {
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
      if (owned_.contains(v) && !callersOwned(place.first->id, place.second, false)) {
        owned_.erase(v);
        changed = true;
      }
    }
    for (const auto &[v, fi] : freshOf_) {
      if (!owned_.contains(v)) {
        continue;
      }
      const CallExpr *c = programCall(analysis_.declInit.lookup(v));
      if (c && !callFresh(fi, c)) {
        owned_.erase(v);
        changed = true;
      }
    }
    for (int fi : scanned) {
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
    if (owned_.contains(v) && callersOwned(place.first->id, place.second, true)) {
      analysis_.ownedParams.insert(v);
    }
  }
}
}
