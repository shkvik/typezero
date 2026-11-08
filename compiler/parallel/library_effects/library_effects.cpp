#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
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

namespace tz::parallel {

using llvm::dyn_cast;

LibraryEffects::LibraryEffects(Analysis &analysis)
    : analysis(analysis) {}

bool LibraryEffects::isEmitterClass(llvm::StringRef cls) {
  return cls == "EventEmitter" || cls == "EventEmitterBase";
}

bool LibraryEffects::isTargetClass(llvm::StringRef cls) {
  return cls == "EventTarget";
}

bool LibraryEffects::isPoolClass(llvm::StringRef cls) {
  return cls == "Pool";
}

bool LibraryEffects::isLockedClass(llvm::StringRef cls) {
  return cls == "ConcurrentMap" || LibraryEffects::isPoolClass(cls);
}

bool LibraryEffects::isContextClass(llvm::StringRef cls) {
  return cls == "AsyncLocalStorage";
}

bool LibraryEffects::readOnlyExpr(const Expr *e) {
  if (!e) {
    return true;
  }
  switch (e->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::Name:
    return true;
  case Expr::Kind::Member:
    return LibraryEffects::readOnlyExpr(llvm::cast<MemberExpr>(e)->object);
  case Expr::Kind::Unary: {
    auto *unary = llvm::cast<UnaryExpr>(e);
    return unary->op != UnaryOp::Delete && LibraryEffects::readOnlyExpr(unary->operand);
  }
  case Expr::Kind::Binary: {
    auto *binary = llvm::cast<BinaryExpr>(e);
    return LibraryEffects::readOnlyExpr(binary->lhs) && LibraryEffects::readOnlyExpr(binary->rhs);
  }
  case Expr::Kind::Conditional: {
    auto *conditional = llvm::cast<ConditionalExpr>(e);
    return LibraryEffects::readOnlyExpr(conditional->cond) &&
           LibraryEffects::readOnlyExpr(conditional->thenExpr) &&
           LibraryEffects::readOnlyExpr(conditional->elseExpr);
  }
  case Expr::Kind::Cast:
    return LibraryEffects::readOnlyExpr(llvm::cast<CastExpr>(e)->operand);
  case Expr::Kind::OptionalChain:
    return LibraryEffects::readOnlyExpr(llvm::cast<OptionalChainExpr>(e)->chain);
  default:
    return false;
  }
}

bool LibraryEffects::readOnlyStmt(const Stmt *s) {
  if (!s) {
    return true;
  }
  if (auto *block = dyn_cast<BlockStmt>(s)) {
    return llvm::all_of(block->body, readOnlyStmt);
  }
  if (auto *ret = dyn_cast<ReturnStmt>(s)) {
    return LibraryEffects::readOnlyExpr(ret->value);
  }
  if (auto *branch = dyn_cast<IfStmt>(s)) {
    return LibraryEffects::readOnlyExpr(branch->cond) &&
           LibraryEffects::readOnlyStmt(branch->thenStmt) &&
           LibraryEffects::readOnlyStmt(branch->elseStmt);
  }
  if (auto *decl = dyn_cast<VarDeclStmt>(s)) {
    return decl->isConst && llvm::all_of(decl->decls, [](const VarDeclarator &d) {
             return !d.destructured() && LibraryEffects::readOnlyExpr(d.init);
           });
  }

  return false;
}

bool LibraryEffects::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();

  return llvm::any_of(this->analysis.libRanges,
                      [&](const std::pair<const char *, const char *> &range) {
                        return at >= range.first && at < range.second;
                      });
}

const FunctionDecl *LibraryEffects::libraryTarget(const CallExpr *c) const {
  if (c->func() && this->analysis.isLibrary(c->func()->decl)) {
    return c->func()->decl;
  }
  if (c->method() && this->analysis.isLibrary(c->method()->decl)) {
    return c->method()->decl;
  }

  return nullptr;
}

void LibraryEffects::libraryCall(const CallExpr *c, const FunctionDecl *decl, const Use &use) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  bool superCtor = c->superCall() && !m;
  std::optional<Ref> self;
  if (superCtor) {
    if (this->analysis.fn->thisOwner) {
      self = Ref{RefKind::This, this->analysis.fn->thisOwner};
    }
  } else if (m && c->method()) {
    this->analysis.exprWalk.expr(m->object);
    self = this->analysis.aliasAnalysis.receiverRef(m->object);
  }
  llvm::StringRef cls = ParallelHelpers::libraryClassName(decl->cls());
  llvm::StringRef name = decl->name;
  llvm::SMLoc at = c->loc();
  for (const Expr *arg : c->args) {
    libraryArg(c, cls, name, arg, at);
  }
  CallRec &rec = this->analysis.codeWalk.record(c, CallKind::Call, use, at);
  rec.call = c;
  if (superCtor) {
    for (const ClassDecl *k = this->analysis.fn->decl->cls(); k; k = k->parent) {
      if (k->init) {
        this->analysis.callWalk.addCallee(rec, k->init, self);
      }
    }
  } else if (c->method() && c->virtualCall()) {
    for (const FunctionDecl *fn : this->analysis.callWalk.methodTargets(decl, true)) {
      this->analysis.callWalk.addCallee(rec, fn, self);
    }
  }
  if (self && !superCtor && !LibraryEffects::isLockedClass(cls) &&
      !LibraryEffects::isContextClass(cls)) {
    libraryState(cls, name, self, !readsOnly(decl), at);
  }
  if (LibraryEffects::isEmitterClass(cls) && name == "emit") {
    dispatch(c, CallKind::Inline, EmitterListeners, use);
  } else if (LibraryEffects::isTargetClass(cls) && name == "dispatchEvent") {
    dispatch(c, CallKind::Inline, TargetListeners, use);
  }
}

void LibraryEffects::libraryNew(const NewExpr *n) {
  llvm::SMLoc at = n->loc();
  for (const Expr *arg : n->ctor() ? n->ctor()->args : n->args) {
    libraryArg(n->ctor(), ParallelHelpers::libraryClassName(n->cls()), "constructor", arg, at);
  }
}

void LibraryEffects::libraryArg(const CallExpr *c, llvm::StringRef cls, llvm::StringRef name,
                                const Expr *arg, llvm::SMLoc at) {
  const Expr *f = ParallelHelpers::strip(arg);
  if (auto *wrap = dyn_cast<CallExpr>(f);
      wrap && wrap->builtin() == Builtin::LibListener && !wrap->args.empty()) {
    f = ParallelHelpers::strip(wrap->args[0]);
  }
  bool fn = f->type && f->type->nonOptional()->isFunction();
  if (fn) {
    if (LibraryEffects::isEmitterClass(cls) &&
        (name == "on" || name == "addListener" || name == "once" || name == "prependListener" ||
         name == "prependOnceListener")) {
      listen(f, EmitterListeners);
    } else if (LibraryEffects::isTargetClass(cls) &&
               (name == "addEventListener" || name == "addFunctionListener")) {
      listen(f, TargetListeners);
    } else if ((cls == "Server" || cls == "Http2Server") && name == "constructor") {
      libraryCallback(c, f, CallKind::Later, true, true, 2);
    } else if (LibraryEffects::isPoolClass(cls) && name == "use") {
      libraryCallback(c, f, CallKind::Later, false, true, 1, poolItemOf(f->type));
    } else if (LibraryEffects::isLockedClass(cls) || LibraryEffects::isContextClass(cls)) {
      libraryCallback(c, f, CallKind::Inline);
    } else {
      libraryCallback(c, f, CallKind::Inline);
      libraryCallback(c, f, CallKind::Later);
    }
    return;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(f)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Expr *v = ParallelHelpers::strip(p.value);
      if (p.spread() && LibraryEffects::isPoolClass(cls)) {
        this->analysis.poolOpen = true;
      }
      if (p.spread() || !v || !v->type || !v->type->nonOptional()->isFunction()) {
        this->analysis.exprWalk.expr(p.value);
        libraryReads(v, at);
        continue;
      }
      if (cls == "ReadableStream" && p.name == "start") {
        libraryCallback(c, v, CallKind::Inline);
      } else if (cls == "ReadableStream" && p.name == "pull") {
        libraryCallback(c, v, CallKind::Later, true);
      } else if (cls == "ReadableStream" && p.name == "cancel") {
        libraryCallback(c, v, CallKind::Later);
      } else if (LibraryEffects::isTargetClass(cls) &&
                 (name == "addEventListener" || name == "addObjectListener") &&
                 p.name == "handleEvent") {
        listen(v, TargetListeners);
      } else if (LibraryEffects::isPoolClass(cls) && name == "constructor") {
        bool create = p.name == "create";
        libraryCallback(c, v, CallKind::Later, true, true, create ? 0 : 1,
                        create ? nullptr : poolItemOf(v->type));
        if (create) {
          poolCreate(v);
        }
      } else {
        libraryCallback(c, v, CallKind::Inline);
        libraryCallback(c, v, CallKind::Later);
      }
    }
    return;
  }
  if (LibraryEffects::isPoolClass(cls) && name == "constructor") {
    this->analysis.poolOpen = true;
  }
  this->analysis.exprWalk.expr(arg);
  libraryReads(f, at);
}

void LibraryEffects::libraryReads(const Expr *e, llvm::SMLoc at) {
  const Type *type = e && e->type ? e->type->nonOptional() : nullptr;
  if (const ClassDecl *cls = type ? type->classDecl() : nullptr; cls && cls->library()) {
    libraryState(ParallelHelpers::libraryClassName(cls), "", this->analysis.aliasAnalysis.refOf(e),
                 false, at);
    return;
  }
  this->analysis.exprWalk.contents(e, false, at);
}

void LibraryEffects::libraryCallback(const CallExpr *c, const Expr *f, CallKind kind, bool interval,
                                     bool handled, unsigned ownArgs, const Type *poolItem) {
  CallRec &rec = this->analysis.codeWalk.record(f, kind, {}, f->loc());
  rec.call = c;
  if (poolItem) {
    rec.poolItem = poolItem;
  }
  rec.interval |= interval;
  rec.handled |= handled;
  rec.ownArgs = std::max(rec.ownArgs, ownArgs);
  if (kind == CallKind::Inline) {
    rec.repeated = true;
  }
  this->analysis.callWalk.addFunctionCallees(f, f, kind);
}

const Type *LibraryEffects::poolItemOf(const Type *fn) {
  const Type *type = fn ? fn->nonOptional() : nullptr;
  if (!type || !type->isFunction() || type->params().empty() || !type->params()[0]) {
    return nullptr;
  }

  return type->params()[0]->nonOptional();
}

void LibraryEffects::poolCreate(const Expr *f) {
  const Type *type = f->type ? f->type->nonOptional() : nullptr;
  const Type *promised =
      type && type->isFunction() && type->ret() ? type->ret()->promised() : nullptr;
  const Type *result = promised ? promised->nonOptional() : nullptr;
  const FunctionDecl *decl = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(f)) {
    decl = fe->fn;
  } else if (auto *n = dyn_cast<NameExpr>(f); n && n->func()) {
    decl = n->func()->decl;
  } else if (n && n->var() && (n->var()->function() || n->var()->lambda())) {
    decl = n->var()->function() ? n->var()->function() : n->var()->lambda();
  }
  if (!result || !result->isResult() || !result->okType() || !decl) {
    this->analysis.poolOpen = true;
    return;
  }
  this->analysis.poolCreates.push_back({result->okType()->nonOptional(), decl});
}

void LibraryEffects::listen(const Expr *f, Channel channel) {
  f = ParallelHelpers::strip(f);
  this->analysis.exprWalk.expr(f);
  const FunctionDecl *decl = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(f)) {
    decl = fe->fn;
  } else if (auto *n = dyn_cast<NameExpr>(f); n && n->func()) {
    decl = n->func()->decl;
  } else if (n && n->var() && (n->var()->function() || n->var()->lambda())) {
    decl = n->var()->function() ? n->var()->function() : n->var()->lambda();
  }
  if (decl && decl->body && !this->analysis.isLibrary(decl)) {
    int fi = this->analysis.codeWalk.discover(decl);
    if (!llvm::is_contained(this->analysis.listeners[channel], fi)) {
      this->analysis.listeners[channel].push_back(fi);
    }
    return;
  }
  if (f->type && f->type->nonOptional()->isFunction() &&
      !llvm::is_contained(this->analysis.listenerTypes[channel], f->type->nonOptional())) {
    this->analysis.listenerTypes[channel].push_back(f->type->nonOptional());
  }
}

void LibraryEffects::dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use) {
  (void)use;
  CallRec &rec = this->analysis.codeWalk.record(c, kind, {}, c->loc());
  rec.call = c;
  rec.channel = channel;
}

bool LibraryEffects::readsOnly(const FunctionDecl *decl) {
  auto found = readsOnly_.find(decl);
  if (found != readsOnly_.end()) {
    return found->second;
  }
  bool reads = decl->body && decl->params.empty() && !decl->isAsync && !decl->isGenerator &&
               LibraryEffects::readOnlyStmt(decl->body);
  readsOnly_[decl] = reads;

  return reads;
}

void LibraryEffects::libraryState(llvm::StringRef cls, llvm::StringRef name,
                                  std::optional<Ref> self, bool write, llvm::SMLoc at) {
  if (!self) {
    return;
  }
  if (LibraryEffects::isEmitterClass(cls) || LibraryEffects::isTargetClass(cls)) {
    bool reads = name == "emit" || name == "dispatchEvent" || name == "listenerCount" ||
                 name == "listeners" || name == "rawListeners" || name == "eventNames";
    this->analysis.codeWalk.touch(LocKind::Field, *self, this->analysis.listenersKey,
                                  write && !reads, at);
    return;
  }
  this->analysis.codeWalk.touch(LocKind::Field, *self, this->analysis.state, write, at);
}

}
