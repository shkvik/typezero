#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/builtin_effects/builtin_effects.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

namespace {

// The built-ins that call the functions given to them, and when; the others
// keep them as values, which code may call later.
std::optional<CallKind> callbackKind(Builtin builtin) {
  switch (builtin) {
  case Builtin::PromiseThen:
  case Builtin::PromiseCatch:
  case Builtin::PromiseFinally:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
  case Builtin::RlQuestionCallback:
  case Builtin::ResultAsyncMap:
  case Builtin::ResultAsyncMapErr:
  case Builtin::ResultAsyncAndThen:
  case Builtin::ResultAsyncOrElse:
  case Builtin::ResultAsyncMatch:
  case Builtin::ResultAsyncUnwrapOr:
  case Builtin::ResultAsyncAndTee:
  case Builtin::ResultAsyncOrTee:
  case Builtin::ResultAsyncFromPromise:
    return CallKind::Later;
  case Builtin::ResultFromThrowable:
  case Builtin::ResultAsyncFromThrowable:
    return std::nullopt; // the function it makes calls them: they are values of its type
  default:
    break;
  }
  if (isResultCallback(builtin) || isIteratorStage(builtin) || isIteratorSink(builtin)) {
    return CallKind::Inline;
  }
  switch (builtin) {
  case Builtin::ArrForEach:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ArrSortWith:
  case Builtin::ArrayFrom:
  case Builtin::MapForEach:
  case Builtin::DictGroupBy:
  case Builtin::StrReplaceRegexFn:
  case Builtin::SafeTry:
  case Builtin::PromiseNew:
    return CallKind::Inline;
  default:
    return std::nullopt;
  }
}

// The built-ins of `callbackKind` that call a function once, rather than for
// each element.
bool callsOnce(Builtin builtin) {
  switch (builtin) {
  case Builtin::PromiseNew:
  case Builtin::SafeTry:
    return true;
  default:
    return isResultCallback(builtin);
  }
}

// Whether a built-in may throw where it is called. Those of promises, timers
// and output do not: an error there rejects a Promise, or comes later.
bool mayThrow(Builtin builtin) {
  if (isPromiseBuiltin(builtin)) {
    return false;
  }
  switch (builtin) {
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
  case Builtin::ClearTimeout:
  case Builtin::ClearImmediate:
    return false;
  default:
    return true;
  }
}

} // namespace

CallWalk::CallWalk(Analysis &analysis) : analysis_(analysis) {}

void CallWalk::addCallee(CallRec &rec, const FunctionDecl *decl, std::optional<Ref> self,
                         bool byType) {
  if (!decl || !decl->body || analysis_.isLibrary(decl)) {
    return;
  }
  unsigned fi = analysis_.codeWalk().discover(decl);
  const FnInfo &callee = analysis_.fns_[fi];
  Mode mode = Mode::Sync;
  switch (rec.kind) {
  case CallKind::Call:
    // Calling an async generator only makes it: the loop that iterates it
    // runs it.
    if (callee.asyncGen) {
      return;
    }
    mode = callee.async ? (rec.await || rec.returned ? Mode::Await : Mode::Spawn) : Mode::Sync;
    break;
  case CallKind::Inline:
    if (callee.asyncGen) {
      return;
    }
    mode = callee.async ? Mode::Spawn : Mode::Sync;
    break;
  case CallKind::Later:
    mode = Mode::Callback;
    break;
  case CallKind::IterSync:
    if (!callee.syncGen) {
      return;
    }
    mode = Mode::Sync;
    break;
  case CallKind::IterAsync:
    if (!callee.asyncGen) {
      return;
    }
    mode = Mode::AsyncIter;
    break;
  }
  Callee c;
  c.fn = fi;
  c.self = self;
  c.mode = mode;
  c.byType = byType;
  if (!llvm::is_contained(rec.callees, c)) {
    rec.callees.push_back(c);
  }
}

std::vector<const FunctionDecl *> CallWalk::methodTargets(const FunctionDecl *method,
                                                          bool virtualCall) {
  std::vector<const FunctionDecl *> targets{method};
  if (!virtualCall || !method) {
    return targets;
  }
  // The methods that override it, down the chain: any of them may run.
  std::vector<const ClassDecl *> pending;
  if (method->cls) {
    for (const ClassDecl *child : method->cls->children) {
      pending.push_back(child);
    }
  }
  while (!pending.empty()) {
    const ClassDecl *cls = pending.back();
    pending.pop_back();
    analysis_.codeWalk().noteClass(cls);
    for (const FunctionDecl *fn : cls->methods) {
      for (const FunctionDecl *up = fn->overrides; up; up = up->overrides) {
        if (up == method) {
          targets.push_back(fn);
          break;
        }
      }
    }
    for (const ClassDecl *child : cls->children) {
      pending.push_back(child);
    }
  }

  return targets;
}

std::optional<Ref> CallWalk::closureSelf(const FunctionDecl *callee, const FnInfo &caller) const {
  const FunctionDecl *owner = analysis_.thisOwnerOf(callee);
  if (!owner) {
    return std::nullopt;
  }
  if (owner == caller.thisOwner) {
    return Ref{RefKind::This, owner};
  }

  return classRef(owner);
}

Ref CallWalk::classRef(const FunctionDecl *owner) const {
  return Ref{RefKind::Type, owner && owner->cls ? owner->cls->type : nullptr};
}

void CallWalk::registerValue(const FunctionDecl *fn, const Type *type) {
  if (!fn || !fn->body || analysis_.isLibrary(fn)) {
    return;
  }
  unsigned fi = analysis_.codeWalk().discover(fn);
  analysis_.valueFns_.insert(fi);
  if (!type || !type->nonOptional()->isFunction()) {
    return;
  }
  std::vector<unsigned> &list = analysis_.values_[type->nonOptional()];
  if (!llvm::is_contained(list, fi)) {
    list.push_back(fi);
  }
}

// Registers the function `e` names, if it names one, as a value of `type`.
bool CallWalk::functionRef(const Expr *e, const Use &use, const Type *type) {
  e = strip(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    if (!use.lambda) {
      registerValue(fe->fn, type);
    }
    return true;
  }
  auto *n = dyn_cast_or_null<NameExpr>(e);
  if (n && n->func) {
    registerValue(n->func->decl, type);
    return true;
  }
  if (n && n->var && (n->var->function || n->var->lambda)) {
    registerValue(n->var->function ? n->var->function : n->var->lambda, type);
    return true;
  }
  auto *m = dyn_cast_or_null<MemberExpr>(e);
  if (m && m->method) {
    for (const FunctionDecl *fn : methodTargets(m->method->decl, m->virtualCall)) {
      registerValue(fn, type);
    }
    return true;
  }

  return false;
}

void CallWalk::call(const CallExpr *c, const Use &use) {
  if (c->builtin != Builtin::None) {
    builtinCall(c, use);
    return;
  }
  if (const FunctionDecl *lib = analysis_.libraryEffects().libraryTarget(c)) {
    analysis_.libraryEffects().libraryCall(c, lib, use);
    return;
  }
  auto *m = dyn_cast<MemberExpr>(c->callee);
  bool superCtor = c->superCall && !m;
  std::optional<Ref> self;
  if (superCtor) {
    if (analysis_.fn_->thisOwner) {
      self = Ref{RefKind::This, analysis_.fn_->thisOwner};
    }
  } else if (m && (c->method || !c->unionMethods.empty())) {
    analysis_.exprWalk().expr(m->object);
    self = analysis_.aliasAnalysis().receiverRef(m->object);
  } else if (!c->func) {
    analysis_.exprWalk().expr(c->callee); // the function value
  }
  for (const Expr *arg : c->args) {
    analysis_.exprWalk().expr(arg);
  }
  // An async function does not throw: its error rejects its Promise.
  const FunctionDecl *direct = c->func ? c->func->decl : c->method ? c->method->decl : nullptr;
  if (!direct || !direct->isAsync || c->virtualCall || superCtor) {
    analysis_.codeWalk().throwPoint();
  }
  CallRec &rec = analysis_.codeWalk().record(c, CallKind::Call, use, c->loc());
  rec.call = c;
  if (superCtor) {
    // The constructor of the class extended, then the initializers of the
    // fields.
    if (c->method) {
      addCallee(rec, c->method->decl, self);
    }
    for (const ClassDecl *k = analysis_.fn_->decl ? analysis_.fn_->decl->cls : nullptr; k;
         k = k->parent) {
      if (k->init) {
        addCallee(rec, k->init, self);
      }
    }
    return;
  }
  if (c->method) {
    for (const FunctionDecl *fn : methodTargets(c->method->decl, c->virtualCall)) {
      addCallee(rec, fn, self);
      // A decorator may have put another function in the method's place.
      if (fn->impl && fn->impl->type && !rec.byType) {
        rec.byType = fn->impl->type;
      }
    }
    return;
  }
  if (!c->unionMethods.empty()) {
    for (const CallExpr::UnionMethod &um : c->unionMethods) {
      if (!um.method) {
        rec.byType = c->callee->type; // a function in a field
        continue;
      }
      for (const FunctionDecl *fn : methodTargets(um.method->decl, um.virtualCall)) {
        addCallee(rec, fn, self);
      }
    }
    return;
  }
  if (c->func) {
    addCallee(rec, c->func->decl, closureSelf(c->func->decl, *analysis_.fn_));
    return;
  }
  // A function value: any function of its type that the program has as a
  // value, and through an object, any method of that name of its classes.
  auto *callee = dyn_cast<NameExpr>(strip(c->callee));
  if (callee && callee->var && analysis_.resolvers_.contains(callee->var)) {
    return;
  }
  if (m && (m->name == "resolve" || m->name == "reject") && m->object->type &&
      m->object->type->nonOptional()->isObject() &&
      m->object->type->nonOptional()->field("promise")) {
    return; // of Promise.withResolvers()
  }
  rec.byType = c->callee->type;
  if (m) {
    rec.byName = m->name;
    rec.byObject = m->object->type;
    rec.bySelf = analysis_.aliasAnalysis().receiverRef(m->object);
  }
}

void CallWalk::builtinCall(const CallExpr *c, const Use &use) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  const Expr *recv = m ? m->object : nullptr;
  if (recv) {
    analysis_.exprWalk().expr(recv);
  }
  Builtin b = c->builtin;
  // Promise.all and allSettled awaited right away wait there for the tasks
  // given to them.
  Use inner;
  if (b == Builtin::PromiseAll || b == Builtin::PromiseAllSettled) {
    inner.join = use.awaited;
  }
  std::optional<CallKind> kind = callbackKind(b);
  bool unknown = !knownBuiltin(b);
  for (size_t i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    bool fn = arg->type && arg->type->nonOptional()->isFunction();
    if (fn && (kind || unknown)) {
      // One the pass does not know may call it now or later.
      if (unknown) {
        functionArg(c, arg, CallKind::Inline, use);
        functionArg(c, arg, CallKind::Later, use);
      } else {
        functionArg(c, arg, *kind, use);
      }
      continue;
    }
    if ((b == Builtin::ResultFromThrowable || b == Builtin::ResultAsyncFromThrowable) && fn &&
        functionRef(arg, {}, c->type)) {
      analysis_.exprWalk().expr(arg);
      continue;
    }
    analysis_.exprWalk().expr(arg, i == 0 ? inner : Use());
  }
  if (mayThrow(b)) {
    analysis_.codeWalk().throwPoint();
  }
  analysis_.builtinCalls().builtinEffects(c, recv);
}

void CallWalk::functionArg(const CallExpr *c, const Expr *arg, CallKind kind, const Use &use) {
  const Expr *f = strip(arg);
  Builtin b = c->builtin;
  // What the callback is given.
  auto *fe = dyn_cast<FunctionExpr>(f);
  if (fe && b == Builtin::PromiseNew) {
    for (size_t i = 0; i < fe->fn->params.size() && i < 2; ++i) {
      if (fe->fn->params[i].sym) {
        analysis_.resolvers_.insert(fe->fn->params[i].sym);
      }
    }
  }
  bool ofArray = c->callee && isa<MemberExpr>(c->callee) &&
                 cast<MemberExpr>(c->callee)->object->type &&
                 cast<MemberExpr>(c->callee)->object->type->nonOptional()->isArray();
  if (fe && ofArray && (b == Builtin::ArrMap || b == Builtin::ArrForEach) &&
      fe->fn->params.size() > 1 && fe->fn->params[1].sym) {
    analysis_.ownIndex_.insert(fe->fn->params[1].sym);
  }
  if (fe && b == Builtin::ArrMap && use.join && !fe->fn->isAsync) {
    analysis_.joinReturns_[fe->fn] = use.join;
  }

  Use recUse;
  if (b == Builtin::ArrMap && fe && fe->fn->isAsync) {
    recUse.join = use.join;
  }
  CallRec &rec = analysis_.codeWalk().record(f, kind, recUse, f->loc());
  rec.call = c;
  if (kind == CallKind::Later) {
    rec.interval |= b == Builtin::SetInterval;
    rec.handled |= b == Builtin::PromiseThen || b == Builtin::PromiseCatch ||
                   b == Builtin::PromiseFinally || isResultAsyncMethod(b) ||
                   b == Builtin::RlQuestionCallback;
  }
  if (kind == CallKind::Inline && !callsOnce(b)) {
    rec.repeated = true;
  }
  addFunctionCallees(f, arg, kind);
}

// The functions that `f`, given to be called, may be: (the strip of `arg`).
void CallWalk::addFunctionCallees(const Expr *f, const Expr *arg, CallKind kind) {
  CallRec &rec =
      analysis_.fn_
          ->calls[analysis_.fn_->callOf.lookup(std::make_pair((const void *)f, unsigned(kind)))];
  if (auto *fe = dyn_cast<FunctionExpr>(f)) {
    analysis_.codeWalk().discover(fe->fn);
    addCallee(rec, fe->fn, closureSelf(fe->fn, *analysis_.fn_));
  } else if (auto *n = dyn_cast<NameExpr>(f); n && n->func) {
    addCallee(rec, n->func->decl, std::nullopt);
  } else if (n && n->var && (n->var->function || n->var->lambda)) {
    const FunctionDecl *decl = n->var->function ? n->var->function : n->var->lambda;
    addCallee(rec, decl, closureSelf(decl, *analysis_.fn_));
  } else if (n && n->var && analysis_.resolvers_.contains(n->var)) {
    // It settles a Promise: no code of the program runs.
  } else if (auto *mem = dyn_cast<MemberExpr>(f); mem && mem->method) {
    analysis_.exprWalk().expr(mem->object);
    CallRec &again =
        analysis_.fn_
            ->calls[analysis_.fn_->callOf.lookup(std::make_pair((const void *)f, unsigned(kind)))];
    for (const FunctionDecl *fn : methodTargets(mem->method->decl, mem->virtualCall)) {
      addCallee(again, fn, analysis_.aliasAnalysis().receiverRef(mem->object));
    }
  } else {
    analysis_.exprWalk().expr(arg);
    CallRec &again =
        analysis_.fn_
            ->calls[analysis_.fn_->callOf.lookup(std::make_pair((const void *)f, unsigned(kind)))];
    again.byType = arg->type;
  }
}

void CallWalk::newExpr(const NewExpr *n) {
  if (n->cls && n->cls->library) {
    analysis_.libraryEffects().libraryNew(n);
    return;
  }
  if (!n->cls) {
    if (n->ctor) {
      call(n->ctor, {});
    } else {
      for (const Expr *arg : n->args) {
        analysis_.exprWalk().expr(arg);
      }
    }
    return;
  }
  analysis_.codeWalk().noteClass(n->cls);
  for (const Expr *arg : n->ctor ? n->ctor->args : n->args) {
    analysis_.exprWalk().expr(arg);
  }
  analysis_.codeWalk().throwPoint();
  CallRec &rec = analysis_.codeWalk().record(n, CallKind::Call, {}, n->loc());
  // Until the constructors give the object away, it is nobody else's: what
  // they do to it is not shared.
  std::optional<Ref> self = analysis_.escapeScan().constructorLeaks(n->cls)
                                ? Ref{RefKind::Site, n}
                                : Ref{RefKind::Local, nullptr};
  if (n->ctor && n->ctor->func) {
    addCallee(rec, n->ctor->func->decl, self);
  } else {
    for (const ClassDecl *k = n->cls; k; k = k->parent) {
      if (!k->constructors.empty()) {
        for (const FunctionDecl *ctor : k->constructors) {
          addCallee(rec, ctor, self);
        }
        break;
      }
    }
  }
  for (const ClassDecl *k = n->cls; k; k = k->parent) {
    if (k->init) {
      addCallee(rec, k->init, self);
    }
  }
}

} // namespace tz::parallel
