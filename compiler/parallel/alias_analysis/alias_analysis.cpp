#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
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
#include "compiler/parallel/parallel_model/parallel_model.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

namespace {

// The built-ins that read or change their receiver and give back no reference
// to it, unless their result is dropped.
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

// The built-ins that only read their arguments, or copy what they hold.
bool onlyReadsArgs(Builtin b) {
  switch (b) {
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
  case Builtin::StringOf:
  case Builtin::JsonStringify:
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
  case Builtin::ArrayIsArray:
  case Builtin::ArrIncludes:
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrConcat:
  case Builtin::ArrayFrom:
  case Builtin::MapHas:
  case Builtin::MapGet:
  case Builtin::DictNew:
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BufferConcat:
  case Builtin::BufferCompare:
  case Builtin::BufferEquals:
  case Builtin::BufferIsBuffer:
  case Builtin::ProcessStdoutWrite:
  case Builtin::ProcessStderrWrite:
    return true;
  default:
    return false;
  }
}

} // namespace

AliasAnalysis::AliasAnalysis(Analysis &analysis) : analysis_(analysis) {}

bool AliasAnalysis::isAllocation(const Expr *e) const {
  e = strip(e);
  if (!e) {
    return false;
  }
  // A function of a built-in module makes what it gives (fetch() a
  // Response, serve() a Server), and so does an async method (text() of a
  // body, read() of a reader); a getter or another method may give an
  // object it has.
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
    // A function of node:crypto that gives a reference makes the object it
    // gives: a hash, a cipher, the bytes of a digest.
    if (c->builtin == Builtin::Crypto && !c->args.empty()) {
      if (auto *number = dyn_cast<IntLitExpr>(strip(c->args[0]));
          number && number->value < std::size(kCryptoFunctions)) {
        char result = kCryptoFunctions[size_t(number->value)].result;
        return result == 'o' || result == 'p';
      }
    }
    // A generator called makes a new iterator.
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
  // An object made right here, which only this expression has.
  if (isAllocation(e)) {
    return Ref{RefKind::Local, nullptr};
  }

  return Ref{RefKind::Type, e->type->nonOptional()};
}

// The object a method is called on: one made right here may still be kept
// by the method.
Ref AliasAnalysis::receiverRef(const Expr *e) {
  const Expr *object = strip(e);
  if (object && isAllocation(object)) {
    return Ref{RefKind::Site, object};
  }

  return refOf(e);
}

// A variable that holds an object its function made, which never leaves the
// function: no other code can touch the object.
bool AliasAnalysis::isLocal(const VarSymbol *v) {
  if (v->isGlobal || v->captured || v->reassigned || !holdsReferences(v->type)) {
    return false;
  }
  auto it = analysis_.declInit_.find(v);
  if (it == analysis_.declInit_.end() || !isAllocation(it->second)) {
    return false;
  }
  if (auto *n = dyn_cast<NewExpr>(it->second); n && n->cls && constructorLeaks(n->cls)) {
    return false;
  }

  return !escapes(v->owner).contains(v);
}

const Analysis::Escapes &AliasAnalysis::escapes(const FunctionDecl *owner) {
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

void AliasAnalysis::scanStmt(const Stmt *s, Analysis::Escapes &out) {
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
    scanUses(cast<ExprStmt>(s)->expr, true, out, /*discarded=*/true);
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
  case Stmt::Kind::Throw:
    scanUses(cast<ThrowStmt>(s)->value, false, out);
    return;
  case Stmt::Kind::Try:
    scanStmt(cast<TryStmt>(s)->block, out);
    scanStmt(cast<TryStmt>(s)->handler, out);
    scanStmt(cast<TryStmt>(s)->finalizer, out);
    return;
  default:
    return;
  }
}

// Finds the variables whose value may leave the function: given to a call,
// stored, returned, captured. `safe` says that the expression is in a place
// that only reads or changes the object it gives (`x.f`, `x[i]`, `x.push(v)`,
// console.log(x)).
void AliasAnalysis::scanUses(const Expr *e, bool safe, Analysis::Escapes &out, bool discarded) {
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
    bool reads = u->op != UnaryOp::NonNull;
    scanUses(u->operand, reads || safe, out, discarded && !reads);
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
      scanUses(m->object, true, out); // a built-in module keeps it from the program
    } else if (m && (c->method || !c->unionMethods.empty())) {
      scanUses(m->object, false, out); // a method may keep `this`
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

// Whether a constructor of the class (or of one up its chain) may give
// `this` away before the object is done: then what it writes may be seen.
bool AliasAnalysis::constructorLeaks(const ClassDecl *cls) {
  auto found = analysis_.leaks_.find(cls);
  if (found != analysis_.leaks_.end()) {
    return found->second;
  }
  analysis_.leaks_[cls] = true; // a cycle of classes is an error anyway
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
    // A Dog[] may be seen as an Animal[] that nothing writes through.
    const Type *x = a->inner()->nonOptional(), *y = b->inner()->nonOptional();
    result = x == y || (x->isObject() && y->isObject() && objectTypesMayAlias(x, y));
  } else if (a->isDict() && b->isDict()) {
    result = a->dictKind() == b->dictKind() && a->keyType() == b->keyType() && a->valueType() &&
             b->valueType() && a->valueType()->isObject() && b->valueType()->isObject() &&
             objectTypesMayAlias(a->valueType(), b->valueType());
  } else if (a->isBytes() && b->isBytes()) {
    result = true; // views of one buffer
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

// The types of the values inside a value of `type`, at any depth: what its
// fields, elements and entries hold, and what those hold in turn.
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
      // An object of a class may be one of a class down its chain, and one
      // of an interface any type declared to be it.
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

// Whether the object of `to` may be the object of `from`, or one inside it;
// with `innerOnly`, only one inside it.
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
    return (a.key == b.key || a.key == analysis_.star_ || b.key == analysis_.star_) && sameObject(a.ref, b.ref);
  }

  return sameObject(a.ref, b.ref);
}

} // namespace tz::parallel
