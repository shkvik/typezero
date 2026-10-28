#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
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

namespace {

bool isEmitterClass(llvm::StringRef cls) {
  return cls == "EventEmitter" || cls == "EventEmitterBase";
}

bool isTargetClass(llvm::StringRef cls) { return cls == "EventTarget" || cls == "AbortSignal"; }

bool isSignalClass(llvm::StringRef cls) { return cls == "AbortSignal" || cls == "AbortController"; }

}

LibraryEffects::LibraryEffects(Analysis &analysis) : analysis_(analysis) {}

bool LibraryEffects::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();

  return llvm::any_of(analysis_.libRanges_,
                      [&](const std::pair<const char *, const char *> &range) {
                        return at >= range.first && at < range.second;
                      });
}

const FunctionDecl *LibraryEffects::libraryTarget(const CallExpr *c) const {
  if (c->func && analysis_.isLibrary(c->func->decl)) {
    return c->func->decl;
  }
  if (c->method && analysis_.isLibrary(c->method->decl)) {
    return c->method->decl;
  }

  return nullptr;
}

void LibraryEffects::libraryCall(const CallExpr *c, const FunctionDecl *decl, const Use &use) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  bool superCtor = c->superCall && !m;
  std::optional<Ref> self;
  if (superCtor) {
    if (analysis_.fn_->thisOwner) {
      self = Ref{RefKind::This, analysis_.fn_->thisOwner};
    }
  } else if (m && c->method) {
    analysis_.exprWalk().expr(m->object);
    self = analysis_.aliasAnalysis().receiverRef(m->object);
  }
  llvm::StringRef cls = libraryClassName(decl->cls);
  llvm::StringRef name = decl->name;
  llvm::SMLoc at = c->loc();
  for (const Expr *arg : c->args) {
    libraryArg(c, cls, name, arg, at);
  }
  CallRec &rec = analysis_.codeWalk().record(c, CallKind::Call, use, at);
  rec.call = c;
  if (superCtor) {
    for (const ClassDecl *k = analysis_.fn_->decl ? analysis_.fn_->decl->cls : nullptr; k;
         k = k->parent) {
      if (k->init) {
        analysis_.callWalk().addCallee(rec, k->init, self);
      }
    }
  } else if (c->method && c->virtualCall) {
    for (const FunctionDecl *fn : analysis_.callWalk().methodTargets(decl, true)) {
      analysis_.callWalk().addCallee(rec, fn, self);
    }
  }
  if (self && !superCtor) {
    libraryState(cls, name, self, true, at);
  }
  if (isEmitterClass(cls) && name == "emit") {
    dispatch(c, CallKind::Inline, EmitterListeners, use);
  } else if ((isTargetClass(cls) && name == "dispatchEvent") ||
             (cls == "AbortController" && name == "abort")) {
    dispatch(c, CallKind::Inline, TargetListeners, use);
  } else if (cls == "AbortSignal" && (name == "timeout" || name == "withAbort")) {
    dispatch(c, CallKind::Later, TargetListeners, use);
  }
}

void LibraryEffects::libraryNew(const NewExpr *n) {
  llvm::SMLoc at = n->loc();
  for (const Expr *arg : n->ctor ? n->ctor->args : n->args) {
    libraryArg(n->ctor, libraryClassName(n->cls), "constructor", arg, at);
  }
}

void LibraryEffects::libraryArg(const CallExpr *c, llvm::StringRef cls, llvm::StringRef name,
                                const Expr *arg, llvm::SMLoc at) {
  const Expr *f = strip(arg);
  if (auto *wrap = dyn_cast<CallExpr>(f);
      wrap && wrap->builtin == Builtin::LibListener && !wrap->args.empty()) {
    f = strip(wrap->args[0]);
  }
  bool fn = f->type && f->type->nonOptional()->isFunction();
  if (fn) {
    if (isEmitterClass(cls) && (name == "on" || name == "addListener" || name == "once" ||
                                name == "prependListener" || name == "prependOnceListener")) {
      listen(f, EmitterListeners);
    } else if (isTargetClass(cls) && (name == "addEventListener" || name == "addFunctionListener" ||
                                      name == "setOnabort")) {
      listen(f, TargetListeners);
    } else if (cls == "Server" && name == "constructor") {
      libraryCallback(c, f, CallKind::Later, true, true);
    } else {
      libraryCallback(c, f, CallKind::Inline);
      libraryCallback(c, f, CallKind::Later);
    }
    return;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(f)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Expr *v = strip(p.value);
      if (p.spread || !v || !v->type || !v->type->nonOptional()->isFunction()) {
        analysis_.exprWalk().expr(p.value);
        libraryReads(v, at);
        continue;
      }
      if (cls == "ReadableStream" && p.name == "start") {
        libraryCallback(c, v, CallKind::Inline);
      } else if (cls == "ReadableStream" && p.name == "pull") {
        libraryCallback(c, v, CallKind::Later, true);
      } else if (cls == "ReadableStream" && p.name == "cancel") {
        libraryCallback(c, v, CallKind::Later);
      } else if (cls == "Server" && name == "listen" && p.name == "onListen") {
        libraryCallback(c, v, CallKind::Inline);
      } else if (isTargetClass(cls) &&
                 (name == "addEventListener" || name == "addObjectListener") &&
                 p.name == "handleEvent") {
        listen(v, TargetListeners);
      } else {
        libraryCallback(c, v, CallKind::Inline);
        libraryCallback(c, v, CallKind::Later);
      }
    }
    return;
  }
  analysis_.exprWalk().expr(arg);
  libraryReads(f, at);
}

void LibraryEffects::libraryReads(const Expr *e, llvm::SMLoc at) {
  const Type *type = e && e->type ? e->type->nonOptional() : nullptr;
  if (const ClassDecl *cls = type ? type->classDecl() : nullptr; cls && cls->library) {
    libraryState(libraryClassName(cls), "", analysis_.aliasAnalysis().refOf(e), false, at);
    return;
  }
  analysis_.exprWalk().contents(e, false, at);
}

void LibraryEffects::libraryCallback(const CallExpr *c, const Expr *f, CallKind kind, bool interval,
                                     bool handled) {
  CallRec &rec = analysis_.codeWalk().record(f, kind, {}, f->loc());
  rec.call = c;
  rec.interval |= interval;
  rec.handled |= handled;
  if (kind == CallKind::Inline) {
    rec.repeated = true;
  }
  analysis_.callWalk().addFunctionCallees(f, f, kind);
}

void LibraryEffects::listen(const Expr *f, Channel channel) {
  f = strip(f);
  analysis_.exprWalk().expr(f);
  const FunctionDecl *decl = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(f)) {
    decl = fe->fn;
  } else if (auto *n = dyn_cast<NameExpr>(f); n && n->func) {
    decl = n->func->decl;
  } else if (n && n->var && (n->var->function || n->var->lambda)) {
    decl = n->var->function ? n->var->function : n->var->lambda;
  }
  if (decl && decl->body && !analysis_.isLibrary(decl)) {
    unsigned fi = analysis_.codeWalk().discover(decl);
    if (!llvm::is_contained(analysis_.listeners_[channel], fi)) {
      analysis_.listeners_[channel].push_back(fi);
    }
    return;
  }
  if (f->type && f->type->nonOptional()->isFunction() &&
      !llvm::is_contained(analysis_.listenerTypes_[channel], f->type->nonOptional())) {
    analysis_.listenerTypes_[channel].push_back(f->type->nonOptional());
  }
}

void LibraryEffects::dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use) {
  (void)use;
  CallRec &rec = analysis_.codeWalk().record(c, kind, {}, c->loc());
  rec.call = c;
  rec.channel = channel;
}

void LibraryEffects::libraryState(llvm::StringRef cls, llvm::StringRef name,
                                  std::optional<Ref> self, bool write, llvm::SMLoc at) {
  if (isSignalClass(cls)) {
    analysis_.resource(Res::Signals, write, at);
    return;
  }
  if (!self) {
    return;
  }
  if (isEmitterClass(cls) || isTargetClass(cls)) {
    bool reads = name == "emit" || name == "dispatchEvent" || name == "listenerCount" ||
                 name == "listeners" || name == "rawListeners" || name == "eventNames" ||
                 name == "getMaxListeners";
    analysis_.codeWalk().touch(LocKind::Field, *self, analysis_.listenersKey_, write && !reads, at);
    return;
  }
  analysis_.codeWalk().touch(LocKind::Field, *self, analysis_.state_, write, at);
}

}
