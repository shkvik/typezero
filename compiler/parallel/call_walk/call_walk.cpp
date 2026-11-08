#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

CallWalk::CallWalk(Analysis &analysis)
    : analysis(analysis) {}

std::optional<CallKind> CallWalk::callbackKind(Builtin builtin) {
  switch (builtin) {
  case Builtin::PromiseThen:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
  case Builtin::RlQuestionCallback:
    return CallKind::Later;
  default:
    break;
  }
  if (BuiltinGroups::isResultCallback(builtin) || BuiltinGroups::isIteratorStage(builtin) ||
      BuiltinGroups::isIteratorSink(builtin)) {
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
  case Builtin::PromiseNew:
    return CallKind::Inline;
  default:
    return std::nullopt;
  }
}

bool CallWalk::callsOnce(Builtin builtin) {
  switch (builtin) {
  case Builtin::PromiseNew:
    return true;
  default:
    return BuiltinGroups::isResultCallback(builtin);
  }
}

void CallWalk::addCallee(CallRec &rec, const FunctionDecl *decl, std::optional<Ref> self,
                         bool byType) {
  if (!decl || !decl->body || this->analysis.isLibrary(decl)) {
    return;
  }
  int fi = this->analysis.codeWalk.discover(decl);
  const FnInfo &callee = *this->analysis.byFunction[fi];
  Mode mode = Mode::Sync;
  switch (rec.kind) {
  case CallKind::Call:
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
  for (int index : rec.edges) {
    const CallEdge &existing = this->analysis.callGraph.edge(index);
    if (existing.to == fi && existing.self == self && existing.mode == mode) {
      return;
    }
  }
  CallEdge edge;
  edge.from = rec.function;
  edge.call = rec.index;
  edge.to = fi;
  edge.self = self;
  edge.mode = mode;
  edge.byType = byType;
  rec.edges.push_back(this->analysis.callGraph.add(edge));
}

std::vector<const FunctionDecl *> CallWalk::methodTargets(const FunctionDecl *method,
                                                          bool virtualCall) {
  std::vector<const FunctionDecl *> targets{method};
  if (!virtualCall || !method) {
    return targets;
  }
  std::vector<const ClassDecl *> pending;
  if (method->cls()) {
    for (const ClassDecl *child : method->cls()->children) {
      pending.push_back(child);
    }
  }
  while (!pending.empty()) {
    const ClassDecl *cls = pending.back();
    pending.pop_back();
    this->analysis.codeWalk.noteClass(cls);
    for (const FunctionDecl *fn : cls->methods) {
      for (const FunctionDecl *up = fn->overrides(); up; up = up->overrides()) {
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
  const FunctionDecl *owner = this->analysis.thisOwnerOf(callee);
  if (!owner) {
    return std::nullopt;
  }
  if (owner == caller.thisOwner) {
    return Ref{RefKind::This, owner};
  }

  return classRef(owner);
}

Ref CallWalk::classRef(const FunctionDecl *owner) const {
  return Ref{RefKind::Type, owner && owner->cls() ? owner->cls()->type : nullptr};
}

void CallWalk::registerValue(const FunctionDecl *fn, const Type *type) {
  if (!fn || !fn->body || this->analysis.isLibrary(fn)) {
    return;
  }
  int fi = this->analysis.codeWalk.discover(fn);
  this->analysis.valueFns.insert(fi);
  if (!type || !type->nonOptional()->isFunction()) {
    return;
  }
  std::vector<int> &list = this->analysis.values[type->nonOptional()];
  if (!llvm::is_contained(list, fi)) {
    list.push_back(fi);
  }
}

void CallWalk::unknownRef(const Expr *e) {
  e = ParallelHelpers::strip(e);
  std::vector<const FunctionDecl *> fns;
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    fns.push_back(fe->fn);
  } else if (auto *n = dyn_cast_or_null<NameExpr>(e); n && n->func()) {
    fns.push_back(n->func()->decl);
  } else if (n && n->var() && !n->var()->reassigned &&
             (n->var()->function() || n->var()->lambda())) {
    fns.push_back(n->var()->function() ? n->var()->function() : n->var()->lambda());
  } else if (auto *m = dyn_cast_or_null<MemberExpr>(e); m && m->method()) {
    fns = methodTargets(m->method()->decl, m->virtualCall());
  } else {
    this->analysis.unknownAnyFn = true;
    return;
  }
  for (const FunctionDecl *fn : fns) {
    if (fn && fn->body && !this->analysis.isLibrary(fn)) {
      this->analysis.unknownFns.insert(this->analysis.codeWalk.discover(fn));
    }
  }
}

bool CallWalk::functionRef(const Expr *e, const Use &use, const Type *type) {
  e = ParallelHelpers::strip(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    if (!use.lambda) {
      registerValue(fe->fn, type);
    }
    return true;
  }
  auto *n = dyn_cast_or_null<NameExpr>(e);
  if (n && n->func()) {
    registerValue(n->func()->decl, type);
    return true;
  }
  if (n && n->var() && (n->var()->function() || n->var()->lambda())) {
    registerValue(n->var()->function() ? n->var()->function() : n->var()->lambda(), type);
    return true;
  }
  auto *m = dyn_cast_or_null<MemberExpr>(e);
  if (m && m->method()) {
    for (const FunctionDecl *fn : methodTargets(m->method()->decl, m->virtualCall())) {
      registerValue(fn, type);
    }
    return true;
  }

  return false;
}

void CallWalk::call(const CallExpr *c, const Use &use) {
  if (c->builtin() != Builtin::None) {
    builtinCall(c, use);
    return;
  }
  if (const FunctionDecl *lib = this->analysis.libraryEffects.libraryTarget(c)) {
    this->analysis.libraryEffects.libraryCall(c, lib, use);
    return;
  }
  auto *m = dyn_cast<MemberExpr>(c->callee);
  bool superCtor = c->superCall() && !m;
  std::optional<Ref> self;
  if (superCtor) {
    if (this->analysis.fn->thisOwner) {
      self = Ref{RefKind::This, this->analysis.fn->thisOwner};
    }
  } else if (m && (c->method() || !c->unionMethods().empty())) {
    this->analysis.exprWalk.expr(m->object);
    self = this->analysis.aliasAnalysis.receiverRef(m->object);
  } else if (!c->func()) {
    this->analysis.exprWalk.expr(c->callee);
  }
  for (const Expr *arg : c->args) {
    this->analysis.exprWalk.expr(arg);
  }
  CallRec &rec = this->analysis.codeWalk.record(c, CallKind::Call, use, c->loc());
  rec.call = c;
  if (superCtor) {
    if (c->method()) {
      addCallee(rec, c->method()->decl, self);
    }
    for (const ClassDecl *k = this->analysis.fn->decl->cls(); k; k = k->parent) {
      if (k->init) {
        addCallee(rec, k->init, self);
      }
    }
    return;
  }
  if (c->method()) {
    for (const FunctionDecl *fn : methodTargets(c->method()->decl, c->virtualCall())) {
      addCallee(rec, fn, self);
      if (fn->impl() && fn->impl()->type && !rec.byType) {
        rec.byType = fn->impl()->type;
      }
    }
    return;
  }
  if (!c->unionMethods().empty()) {
    for (const UnionMethod &um : c->unionMethods()) {
      if (!um.method) {
        rec.byType = c->callee->type;
        continue;
      }
      for (const FunctionDecl *fn : methodTargets(um.method->decl, um.virtualCall)) {
        addCallee(rec, fn, self);
      }
    }
    return;
  }
  if (c->func()) {
    addCallee(rec, c->func()->decl, closureSelf(c->func()->decl, *this->analysis.fn));
    return;
  }
  auto *callee = dyn_cast<NameExpr>(ParallelHelpers::strip(c->callee));
  if (callee && callee->var() && this->analysis.resolvers.contains(callee->var())) {
    return;
  }
  if (m && (m->name == "resolve" || m->name == "reject") && m->object->type &&
      m->object->type->nonOptional()->isObject() &&
      m->object->type->nonOptional()->field("promise")) {
    return;
  }
  rec.byType = c->callee->type;
  if (m) {
    rec.byName = m->name;
    rec.byObject = m->object->type;
    rec.bySelf = this->analysis.aliasAnalysis.receiverRef(m->object);
  }
  const FunctionDecl *fn = this->analysis.fn->decl;
  if (callee && callee->var() && !callee->var()->reassigned && fn &&
      llvm::any_of(fn->params, [&](const Param &p) { return p.sym == callee->var(); })) {
    rec.viaParam = callee->var();
  }
}

void CallWalk::builtinCall(const CallExpr *c, const Use &use) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  const Expr *recv = m ? m->object : nullptr;
  if (recv) {
    this->analysis.exprWalk.expr(recv);
  }
  Builtin b = c->builtin();
  Use inner;
  if (b == Builtin::PromiseAll) {
    inner.join = use.awaited;
  }
  std::optional<CallKind> kind = CallWalk::callbackKind(b);
  bool unknown = !ParallelHelpers::knownBuiltin(b);
  for (size_t i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    bool fn = arg->type && arg->type->nonOptional()->isFunction();
    if (fn && (kind || unknown)) {
      if (unknown) {
        functionArg(c, arg, CallKind::Inline, use);
        functionArg(c, arg, CallKind::Later, use);
      } else {
        functionArg(c, arg, *kind, use);
      }
      continue;
    }
    if (b == Builtin::ReflectApply && i == 0) {
      this->analysis.exprWalk.expr(ParallelHelpers::strip(arg));
      continue;
    }
    this->analysis.exprWalk.expr(arg, i == 0 ? inner : Use());
  }
  this->analysis.builtinCalls.builtinEffects(c, recv);
}

void CallWalk::functionArg(const CallExpr *c, const Expr *arg, CallKind kind, const Use &use) {
  const Expr *f = ParallelHelpers::strip(arg);
  Builtin b = c->builtin();
  auto *fe = dyn_cast<FunctionExpr>(f);
  if (fe && b == Builtin::PromiseNew) {
    for (size_t i = 0; i < fe->fn->params.size() && i < 2; ++i) {
      if (fe->fn->params[i].sym) {
        this->analysis.resolvers.insert(fe->fn->params[i].sym);
      }
    }
  }
  bool ofArray = c->callee && isa<MemberExpr>(c->callee) &&
                 cast<MemberExpr>(c->callee)->object->type &&
                 cast<MemberExpr>(c->callee)->object->type->nonOptional()->isArray();
  if (fe && ofArray && (b == Builtin::ArrMap || b == Builtin::ArrForEach) &&
      fe->fn->params.size() > 1 && fe->fn->params[1].sym) {
    this->analysis.ownIndex.insert(fe->fn->params[1].sym);
  }
  if (fe && b == Builtin::ArrMap && use.join && !fe->fn->isAsync) {
    this->analysis.joinReturns[fe->fn] = use.join;
  }

  Use recUse;
  if (b == Builtin::ArrMap && fe && fe->fn->isAsync) {
    recUse.join = use.join;
  }
  CallRec &rec = this->analysis.codeWalk.record(f, kind, recUse, f->loc());
  rec.call = c;
  if (kind == CallKind::Later) {
    rec.interval |= b == Builtin::SetInterval;
    rec.handled |= b == Builtin::PromiseThen || b == Builtin::RlQuestionCallback;
  }
  if (kind == CallKind::Inline && !CallWalk::callsOnce(b)) {
    rec.repeated = true;
  }
  addFunctionCallees(f, arg, kind);
}

void CallWalk::addFunctionCallees(const Expr *f, const Expr *arg, CallKind kind) {
  CallRec &rec = this->analysis.fn->calls[this->analysis.fn->callOf.lookup(
      std::make_pair((const void *)f, unsigned(kind)))];
  if (auto *fe = dyn_cast<FunctionExpr>(f)) {
    this->analysis.codeWalk.discover(fe->fn);
    addCallee(rec, fe->fn, closureSelf(fe->fn, *this->analysis.fn));
  } else if (auto *n = dyn_cast<NameExpr>(f); n && n->func()) {
    addCallee(rec, n->func()->decl, std::nullopt);
  } else if (n && n->var() && (n->var()->function() || n->var()->lambda())) {
    const FunctionDecl *decl = n->var()->function() ? n->var()->function() : n->var()->lambda();
    addCallee(rec, decl, closureSelf(decl, *this->analysis.fn));
  } else if (n && n->var() && this->analysis.resolvers.contains(n->var())) {
  } else if (auto *mem = dyn_cast<MemberExpr>(f); mem && mem->method()) {
    this->analysis.exprWalk.expr(mem->object);
    CallRec &again = this->analysis.fn->calls[this->analysis.fn->callOf.lookup(
        std::make_pair((const void *)f, unsigned(kind)))];
    for (const FunctionDecl *fn : methodTargets(mem->method()->decl, mem->virtualCall())) {
      addCallee(again, fn, this->analysis.aliasAnalysis.receiverRef(mem->object));
    }
  } else {
    this->analysis.exprWalk.expr(arg);
    CallRec &again = this->analysis.fn->calls[this->analysis.fn->callOf.lookup(
        std::make_pair((const void *)f, unsigned(kind)))];
    again.byType = arg->type;
  }
}

void CallWalk::newExpr(const NewExpr *n) {
  if (n->cls() && n->cls()->library()) {
    this->analysis.libraryEffects.libraryNew(n);
    return;
  }
  if (!n->cls()) {
    if (n->ctor()) {
      call(n->ctor(), {});
    } else {
      for (const Expr *arg : n->args) {
        this->analysis.exprWalk.expr(arg);
      }
    }
    return;
  }
  this->analysis.codeWalk.noteClass(n->cls());
  for (const Expr *arg : n->ctor() ? n->ctor()->args : n->args) {
    this->analysis.exprWalk.expr(arg);
  }
  CallRec &rec = this->analysis.codeWalk.record(n, CallKind::Call, {}, n->loc());
  std::optional<Ref> self = this->analysis.escapeScan.constructorLeaks(n->cls())
                                ? Ref{RefKind::Site, n}
                                : Ref{RefKind::Local, nullptr};
  if (n->ctor() && n->ctor()->func()) {
    addCallee(rec, n->ctor()->func()->decl, self);
  } else {
    for (const ClassDecl *k = n->cls(); k; k = k->parent) {
      if (!k->constructors.empty()) {
        for (const FunctionDecl *ctor : k->constructors) {
          addCallee(rec, ctor, self);
        }
        break;
      }
    }
  }
  for (const ClassDecl *k = n->cls(); k; k = k->parent) {
    if (k->init) {
      addCallee(rec, k->init, self);
    }
  }
}

}
