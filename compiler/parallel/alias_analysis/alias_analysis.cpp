#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/escape_scan/escape_scan.h"
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

using llvm::dyn_cast;

using llvm::dyn_cast_or_null;

using llvm::isa;

AliasAnalysis::AliasAnalysis(Analysis &analysis) : analysis_(analysis) {}

bool AliasAnalysis::isAllocation(const Expr *e) const {
  e = strip(e);
  if (!e) {
    return false;
  }
  const Expr *made = e;
  if (auto *a = dyn_cast<AwaitExpr>(e)) {
    made = strip(a->value);
  }
  if (auto *c = dyn_cast_or_null<CallExpr>(made)) {
    if (const FunctionDecl *lib = analysis_.libraryEffects().libraryTarget(c)) {
      return made == e ? !lib->cls : lib->isAsync;
    }
  }
  if (isa<ArrayLitExpr, ObjectLitExpr, RegexLitExpr>(e)) {
    return true;
  }
  if (auto *n = dyn_cast<NewExpr>(e)) {
    return n->cls || (n->type && n->type->isArray());
  }
  if (auto *c = dyn_cast<CallExpr>(e)) {
    if (makesNewObject(c->builtin)) {
      return true;
    }
    if (c->builtin == Builtin::Crypto && !c->args.empty()) {
      if (auto *number = dyn_cast<IntLitExpr>(strip(c->args[0]));
          number && number->value < std::size(kCryptoFunctions)) {
        char result = kCryptoFunctions[size_t(number->value)].result;
        return result == 'o' || result == 'p';
      }
    }
    const FunctionDecl *fn = c->func ? c->func->decl : c->method ? c->method->decl : nullptr;
    return fn && fn->isGenerator && !fn->safeTry;
  }

  return false;
}

Ref AliasAnalysis::refOf(const Expr *e) {
  e = strip(e);
  if (!e || !e->type) {
    return Ref{RefKind::Type, nullptr};
  }
  if (auto *n = dyn_cast<NameExpr>(e); n && n->var) {
    const VarSymbol *v = n->var;
    if (v->owner && v->owner->thisVar == v) {
      return Ref{RefKind::This, v->owner};
    }
    auto it = analysis_.declInit_.find(v);
    if (it != analysis_.declInit_.end() && !v->reassigned && isAllocation(it->second)) {
      if (isLocal(v)) {
        return Ref{RefKind::Local, nullptr};
      }
      return Ref{RefKind::Site, it->second};
    }
  }
  if (isAllocation(e)) {
    return Ref{RefKind::Local, nullptr};
  }

  return Ref{RefKind::Type, e->type->nonOptional()};
}

Ref AliasAnalysis::receiverRef(const Expr *e) {
  const Expr *object = strip(e);
  if (object && isAllocation(object)) {
    return Ref{RefKind::Site, object};
  }

  return refOf(e);
}

bool AliasAnalysis::isLocal(const VarSymbol *v) {
  if (v->isGlobal || v->captured || v->reassigned || !holdsReferences(v->type)) {
    return false;
  }
  auto it = analysis_.declInit_.find(v);
  if (it == analysis_.declInit_.end() || !isAllocation(it->second)) {
    return false;
  }
  if (auto *n = dyn_cast<NewExpr>(it->second);
      n && n->cls && analysis_.escapeScan().constructorLeaks(n->cls)) {
    return false;
  }

  return !analysis_.escapeScan().escapes(v->owner).contains(v);
}

const Type *AliasAnalysis::typeOfRef(Ref r) const {
  switch (r.kind) {
  case RefKind::Site: {
    const Type *t = static_cast<const Expr *>(r.ptr)->type;
    return t ? t->nonOptional() : nullptr;
  }
  case RefKind::Type:
    return static_cast<const Type *>(r.ptr);
  case RefKind::This: {
    auto *owner = static_cast<const FunctionDecl *>(r.ptr);
    return owner && owner->cls ? owner->cls->type : nullptr;
  }
  case RefKind::Local:
    return nullptr;
  }

  return nullptr;
}

bool AliasAnalysis::sameObject(Ref a, Ref b) {
  if (a.kind == RefKind::Site && b.kind == RefKind::Site) {
    return a.ptr == b.ptr;
  }

  return typesMayAlias(typeOfRef(a), typeOfRef(b));
}

bool AliasAnalysis::typesMayAlias(const Type *a, const Type *b) {
  if (!a || !b) {
    return true;
  }
  a = a->nonOptional();
  b = b->nonOptional();
  if (a == b) {
    return true;
  }
  auto key = std::make_pair(std::min(a, b, std::less<const Type *>()),
                            std::max(a, b, std::less<const Type *>()));
  auto found = analysis_.alias_.find(key);
  if (found != analysis_.alias_.end()) {
    return found->second;
  }
  bool result = false;
  auto any = [](const Type *t) { return t->isUnknown() || t->isAnyObject() || t->isParam(); };
  if (any(a) || any(b)) {
    result = holdsReferences(a) && holdsReferences(b);
  } else if (a->isUnion() || b->isUnion()) {
    const Type *u = a->isUnion() ? a : b, *other = a->isUnion() ? b : a;
    analysis_.alias_[key] = false;
    for (const Type *m : u->members()) {
      if (typesMayAlias(m, other)) {
        result = true;
        break;
      }
    }
  } else if (a->isArray() && b->isArray()) {
    const Type *x = a->inner()->nonOptional(), *y = b->inner()->nonOptional();
    result = x == y || (x->isObject() && y->isObject() && objectTypesMayAlias(x, y));
  } else if (a->isDict() && b->isDict()) {
    result = a->dictKind() == b->dictKind() && a->keyType() == b->keyType() && a->valueType() &&
             b->valueType() && a->valueType()->isObject() && b->valueType()->isObject() &&
             objectTypesMayAlias(a->valueType(), b->valueType());
  } else if (a->isBytes() && b->isBytes()) {
    result = true;
  } else if (a->isObject() && b->isObject()) {
    result = objectTypesMayAlias(a, b);
  }

  return analysis_.alias_[key] = result;
}

bool AliasAnalysis::objectTypesMayAlias(const Type *a, const Type *b) {
  if (a == b || a->isSubtypeOf(b) || b->isSubtypeOf(a)) {
    return true;
  }
  for (const Type *t : analysis_.program_.subtypes) {
    if (t->isSubtypeOf(a) && t->isSubtypeOf(b)) {
      return true;
    }
  }
  for (const auto &[x, y] : analysis_.program_.views) {
    if ((x == a && y == b) || (x == b && y == a)) {
      return true;
    }
  }

  return false;
}

const std::vector<const Type *> &AliasAnalysis::inside(const Type *type) {
  auto found = analysis_.reach_.find(type);
  if (found != analysis_.reach_.end()) {
    return found->second;
  }
  std::vector<const Type *> out;
  std::set<const Type *> seen;
  std::vector<const Type *> pending;
  auto add = [&](const Type *t) {
    if (t && seen.insert(t).second) {
      pending.push_back(t);
    }
  };
  auto fieldsOf = [&](const Type *t) {
    for (const Field &f : t->fields()) {
      add(f.type);
    }
  };
  std::function<void(const Type *)> parts = [&](const Type *t) {
    if (!t) {
      return;
    }
    switch (t->kind()) {
    case Type::Kind::Optional:
      parts(t->inner());
      break;
    case Type::Kind::Union:
      for (const Type *m : t->members()) {
        parts(m);
      }
      break;
    case Type::Kind::Array:
    case Type::Kind::Promise:
      add(t->inner());
      break;
    case Type::Kind::Dict:
      add(t->keyType());
      add(t->valueType());
      break;
    case Type::Kind::Result:
      add(t->okType());
      add(t->errType());
      break;
    case Type::Kind::Iterator:
      add(t->yielded());
      break;
    case Type::Kind::Object: {
      fieldsOf(t);
      std::vector<const ClassDecl *> down;
      if (const ClassDecl *cls = t->classDecl()) {
        down.assign(cls->children.begin(), cls->children.end());
      }
      while (!down.empty()) {
        const ClassDecl *cls = down.back();
        down.pop_back();
        if (cls->type) {
          fieldsOf(cls->type);
        }
        down.insert(down.end(), cls->children.begin(), cls->children.end());
      }
      for (const Type *sub : analysis_.program_.subtypes) {
        if (sub != t && sub->isSubtypeOf(t)) {
          fieldsOf(sub);
        }
      }
      break;
    }
    default:
      break;
    }
  };
  parts(type);
  while (!pending.empty()) {
    const Type *t = pending.back();
    pending.pop_back();
    out.push_back(t);
    parts(t);
  }

  return analysis_.reach_[type] = std::move(out);
}

bool AliasAnalysis::mayReach(Ref from, Ref to, bool innerOnly) {
  if (!innerOnly && sameObject(from, to)) {
    return true;
  }
  const Type *source = typeOfRef(from), *target = typeOfRef(to);
  if (!source || !target) {
    return true;
  }
  for (const Type *t : inside(source)) {
    if (holdsReferences(t) && typesMayAlias(t, target)) {
      return true;
    }
  }

  return false;
}

bool AliasAnalysis::locsMayMeet(const Loc &a, const Loc &b) {
  if (a.kind == LocKind::Resource || b.kind == LocKind::Resource) {
    if (a.kind != b.kind) {
      return false;
    }
    auto output = [](Res r) { return r == Res::Stdout || r == Res::Stderr; };
    return a.res == b.res || (output(a.res) && output(b.res));
  }
  if (a.kind == LocKind::Var || b.kind == LocKind::Var) {
    return a.kind == b.kind && a.key == b.key;
  }
  if (a.kind == LocKind::Contents || b.kind == LocKind::Contents) {
    const Loc &c = a.kind == LocKind::Contents ? a : b;
    const Loc &o = a.kind == LocKind::Contents ? b : a;
    return mayReach(c.ref, o.ref, c.key == analysis_.inner_) ||
           (o.kind == LocKind::Contents && mayReach(o.ref, c.ref, o.key == analysis_.inner_));
  }
  if (a.kind == LocKind::Field || b.kind == LocKind::Field) {
    if (a.kind != b.kind) {
      return false;
    }
    return (a.key == b.key || a.key == analysis_.star_ || b.key == analysis_.star_) &&
           sameObject(a.ref, b.ref);
  }

  return sameObject(a.ref, b.ref);
}

}
