#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

CodeWalk::CodeWalk(Analysis &analysis)
    : analysis_(analysis) {}

int CodeWalk::discover(const FunctionDecl *fn) {
  if (analysis_.byFunction[fn->id] != nullptr) {
    return fn->id;
  }
  FnInfo &info = analysis_.records.emplace_back();
  analysis_.byFunction[fn->id] = &info;
  
  info.decl = fn;
  info.async = fn->isAsync && !fn->isGenerator;
  info.asyncGen = fn->isAsync && fn->isGenerator;
  info.syncGen = !fn->isAsync && fn->isGenerator;
  info.thisOwner = analysis_.thisOwnerOf(fn);

  if (fn->cls()) {
    noteClass(fn->cls());
  }
  info.segs.emplace_back();
  info.segs[0].loc = fn->loc();
  analysis_.worklist.push_back(fn->id);

  return fn->id;
}

void CodeWalk::noteClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    if (analysis_.classSeen.insert(cls).second) {
      analysis_.classes.push_back(cls);
    }
  }
}

void CodeWalk::walk(int fi) {
  analysis_.fi = fi;
  analysis_.fn = analysis_.byFunction[fi];
  analysis_.cur = {0};
  analysis_.targets.clear();
  analysis_.openJoins.clear();
  analysis_.loops = 0;
  analysis_.label = {};
  analysis_.anchors.clear();
  const FunctionDecl *decl = analysis_.fn->decl;
  if (decl == analysis_.module) {
    for (const Stmt *item : analysis_.program.items) {
      if (!isa<FunctionDecl>(item) && !analysis_.libraryEffects.inLibrary(item->loc())) {
        anchored(item);
      }
    }
    return;
  }
  for (const Param &p : decl->params) {
    if (p.defaultValue()) {
      analysis_.exprWalk.expr(p.defaultValue());
    }
  }
  stmt(decl->body);
}

unsigned CodeWalk::segment(const Node *at, SegKind kind, llvm::SMLoc loc) {
  auto [it, inserted] = analysis_.fn->segOf.try_emplace(at, unsigned(analysis_.fn->segs.size()));
  if (inserted) {
    Segment &seg = analysis_.fn->segs.emplace_back();
    seg.kind = kind;
    seg.at = at;
    seg.loc = loc;
  }

  return it->second;
}

CallRec &CodeWalk::record(const void *key, CallKind kind, const Use &use, llvm::SMLoc loc) {
  auto [it, inserted] = analysis_.fn->callOf.try_emplace(std::make_pair(key, unsigned(kind)),
                                                         unsigned(analysis_.fn->calls.size()));
  if (inserted) {
    CallRec &rec = analysis_.fn->calls.emplace_back();
    rec.function = analysis_.fi;
    rec.index = int(it->second);
    rec.kind = kind;
    rec.site = static_cast<const Node *>(key);
    rec.loc = loc;
    rec.await = use.awaited;
    rec.returned = use.returned;
    rec.tail = use.tail;
    rec.voided = use.voided;
    rec.join = use.join;
  }
  CallRec &rec = analysis_.fn->calls[it->second];
  rec.repeated |= analysis_.loops > 0;
  rec.inLoop |= analysis_.loops > 0;
  merge(rec.segs, analysis_.cur);

  return rec;
}

void CodeWalk::touch(LocKind kind, Ref ref, const void *key, bool write, llvm::SMLoc at, Res res) {
  if (ref.kind == RefKind::Local || analysis_.cur.empty()) {
    return;
  }
  Access a;
  a.loc.kind = kind;
  a.loc.ref = ref;
  a.loc.key = key;
  a.loc.res = res;
  a.write = write;
  a.at = at;
  if (!analysis_.anchors.empty()) {
    a.anchor = analysis_.anchors.back()->loc();
  }
  for (unsigned s : analysis_.cur) {
    analysis_.fn->segs[s].effects.insert(a);
  }
}

void CodeWalk::var(const VarSymbol *v, bool write, llvm::SMLoc at) {
  if (v && analysis_.shared(v)) {
    touch(LocKind::Var, Ref(), v, write, at);
  }
}

void CodeWalk::joinPoint() {
  for (OpenJoin &j : analysis_.openJoins) {
    merge(j.active, analysis_.cur);
  }
}

void CodeWalk::moveTo(unsigned seg) {
  for (const Stmt *anchor : analysis_.anchors) {
    analysis_.partedAnchors.insert(anchor->loc().getPointer());
  }
  for (unsigned s : analysis_.cur) {
    analysis_.fn->succ.insert({s, seg});
  }
  analysis_.cur = {seg};
  joinPoint();
}

void CodeWalk::loop(llvm::StringRef label, llvm::function_ref<void()> head,
                    llvm::function_ref<void()> body, llvm::function_ref<void()> step,
                    bool headFirst) {
  Active entry = analysis_.cur, exits;
  ++analysis_.loops;
  while (true) {
    analysis_.cur = entry;
    if (headFirst) {
      head();
      merge(exits, analysis_.cur);
    }
    analysis_.targets.push_back({label, true, false, {}, {}});
    body();
    Target target = std::move(analysis_.targets.back());
    analysis_.targets.pop_back();
    merge(analysis_.cur, target.continues);
    if (step) {
      step();
    }
    if (!headFirst) {
      head();
      merge(exits, analysis_.cur);
    }
    merge(exits, target.breaks);
    Active next = entry;
    merge(next, analysis_.cur);
    if (next == entry) {
      break;
    }
    entry = std::move(next);
  }
  --analysis_.loops;
  analysis_.cur = std::move(exits);
}

void CodeWalk::anchored(const Stmt *s) {
  if (!s) {
    return;
  }
  analysis_.anchors.push_back(s);
  stmt(s);
  analysis_.anchors.pop_back();
}

void CodeWalk::stmt(const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *x : cast<BlockStmt>(s)->body) {
      anchored(x);
    }
    return;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls) {
      declarator(d);
    }
    return;
  case Stmt::Kind::Function:
    return;
  case Stmt::Kind::Expr:
    analysis_.exprWalk.expr(cast<ExprStmt>(s)->expr);
    return;
  case Stmt::Kind::If: {
    auto *i = cast<IfStmt>(s);
    analysis_.exprWalk.expr(i->cond);
    Active before = analysis_.cur;
    stmt(i->thenStmt);
    Active then = std::move(analysis_.cur);
    analysis_.cur = std::move(before);
    stmt(i->elseStmt);
    merge(analysis_.cur, then);
    return;
  }
  case Stmt::Kind::While: {
    auto *w = cast<WhileStmt>(s);
    loop(
        std::exchange(analysis_.label, {}), [&] { analysis_.exprWalk.expr(w->cond); },
        [&] { stmt(w->body); }, {}, true);
    return;
  }
  case Stmt::Kind::DoWhile: {
    auto *w = cast<DoWhileStmt>(s);
    loop(
        std::exchange(analysis_.label, {}), [&] { analysis_.exprWalk.expr(w->cond); },
        [&] { stmt(w->body); }, {}, false);
    return;
  }
  case Stmt::Kind::For: {
    auto *f = cast<ForStmt>(s);
    llvm::StringRef label = std::exchange(analysis_.label, {});
    stmt(f->init);
    loop(
        label, [&] { analysis_.exprWalk.expr(f->cond); }, [&] { stmt(f->body); },
        [&] { analysis_.exprWalk.expr(f->step); }, true);
    return;
  }
  case Stmt::Kind::ForOf:
    forOf(cast<ForOfStmt>(s), std::exchange(analysis_.label, {}));
    return;
  case Stmt::Kind::Switch: {
    auto *sw = cast<SwitchStmt>(s);
    llvm::StringRef label = std::exchange(analysis_.label, {});
    analysis_.exprWalk.expr(sw->discriminant);
    Active disc = analysis_.cur, fall;
    bool hasDefault = false;
    analysis_.targets.push_back({label, false, false, {}, {}});
    for (const SwitchCase &c : sw->cases) {
      analysis_.cur = disc;
      merge(analysis_.cur, fall);
      if (c.test) {
        analysis_.exprWalk.expr(c.test);
      } else {
        hasDefault = true;
      }
      for (const Stmt *x : c.body) {
        anchored(x);
      }
      fall = analysis_.cur;
    }
    Target target = std::move(analysis_.targets.back());
    analysis_.targets.pop_back();
    analysis_.cur = std::move(fall);
    merge(analysis_.cur, target.breaks);
    if (!hasDefault && !sw->exhaustive) {
      merge(analysis_.cur, disc);
    }
    return;
  }
  case Stmt::Kind::Labeled: {
    auto *l = cast<LabeledStmt>(s);
    if (isa<WhileStmt, DoWhileStmt, ForStmt, ForOfStmt, SwitchStmt>(l->body)) {
      analysis_.label = l->label;
      stmt(l->body);
      return;
    }
    analysis_.targets.push_back({l->label, false, true, {}, {}});
    stmt(l->body);
    Target target = std::move(analysis_.targets.back());
    analysis_.targets.pop_back();
    merge(analysis_.cur, target.breaks);
    return;
  }
  case Stmt::Kind::Return: {
    auto *r = cast<ReturnStmt>(s);
    Use use;
    use.returned = analysis_.fn->async;
    if (!analysis_.fn->async) {
      use.join = analysis_.joinReturns.lookup(analysis_.fn->decl);
      use.tail = !analysis_.fn->syncGen && !analysis_.fn->asyncGen;
    }
    analysis_.exprWalk.expr(r->value, use);
    analysis_.cur.clear();
    return;
  }
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue: {
    bool isBreak = isa<BreakStmt>(s);
    llvm::StringRef label = isBreak ? cast<BreakStmt>(s)->label : cast<ContinueStmt>(s)->label;
    for (auto it = analysis_.targets.rbegin(); it != analysis_.targets.rend(); ++it) {
      bool matches = !label.empty() ? it->label == label : isBreak ? !it->block : it->loop;
      if (!matches) {
        continue;
      }
      merge(isBreak ? it->breaks : it->continues, analysis_.cur);
      break;
    }
    analysis_.cur.clear();
    return;
  }
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(s);
    if (cls->builtin() || cls->generic()) {
      return;
    }
    for (const ClassField &f : cls->fields) {
      if (f.isStatic() && f.init) {
        analysis_.exprWalk.expr(f.init);
      }
    }
    for (const FunctionDecl *method : cls->methods) {
      if (method->impl()) {
        analysis_.callWalk.registerValue(method, method->impl()->type);
      }
    }
    for (const Stmt *x : cls->decorations) {
      stmt(x);
    }
    return;
  }
  default:
    return;
  }
}

void CodeWalk::declarator(const VarDeclarator &d) {
  if (d.alias || !d.init) {
    return;
  }
  Use use;
  if (d.sym && d.sym->lambda()) {
    use.lambda = true;
  }
  analysis_.exprWalk.expr(d.init, use);
  if (!d.sym) {
    return;
  }
  const Expr *init = strip(d.init);
  analysis_.declInit[d.sym] = init;
  if (!d.sym->reassigned && analysis_.aliasAnalysis.isAllocation(init)) {
    analysis_.siteName.try_emplace(init, d.name);
  }
  if (d.sym->capturedEarly()) {
    var(d.sym, true, d.loc);
  }
}

void CodeWalk::forOf(const ForOfStmt *f, llvm::StringRef label) {
  analysis_.exprWalk.expr(f->iterable);
  loop(
      label,
      [&] {
        iterate(f->iterable, f);
        if (f->isAwait) {
          moveTo(segment(f, SegKind::ForAwait, f->loc()));
        }
      },
      [&] { stmt(f->body); }, {}, true);
}

void CodeWalk::iterate(const Expr *iterable, const Node *site) {
  if (!iterable || !iterable->type) {
    return;
  }
  const Type *type = iterable->type->nonOptional();
  llvm::SMLoc at = iterable->loc();
  const Expr *source = strip(iterable);
  if (type->runtimeKind() == Type::Runtime::Readline ||
      type->runtimeKind() == Type::Runtime::ReadlinePromises) {
    analysis_.resource(Res::Stdin, true, at);
    return;
  }
  if (type->isAsyncIterator() || type->isIterator()) {
    bool asyncIter = type->isAsyncIterator();
    CallRec &rec = record(site, asyncIter ? CallKind::IterAsync : CallKind::IterSync, {}, at);
    auto *c = dyn_cast<CallExpr>(source);
    if (c && analysis_.libraryEffects.libraryTarget(c)) {
      const FunctionDecl *lib = analysis_.libraryEffects.libraryTarget(c);
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (m && c->method()) {
        analysis_.libraryEffects.libraryState(libraryClassName(lib->cls()), lib->name,
                                              analysis_.aliasAnalysis.receiverRef(m->object), true,
                                              at);
      }
      return;
    }
    if (c && (c->func() || c->method())) {
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (c->method()) {
        for (const FunctionDecl *fn :
             analysis_.callWalk.methodTargets(c->method()->decl, c->virtualCall())) {
          analysis_.callWalk.addCallee(
              rec, fn,
              m ? std::optional<Ref>(analysis_.aliasAnalysis.receiverRef(m->object))
                : std::nullopt);
        }
      } else {
        analysis_.callWalk.addCallee(
            rec, c->func()->decl, analysis_.callWalk.closureSelf(c->func()->decl, *analysis_.fn));
      }
      return;
    }
    if (c && c->builtin() != Builtin::None && makesNewObject(c->builtin())) {
      return;
    }
    rec.byType = type;
    analysis_.exprWalk.elements(iterable, true, at);
    return;
  }
  if (type->isArray() || type->isDict() || type->isTypedArray() || type->isJson()) {
    analysis_.exprWalk.elements(iterable, false, at);
  }
}

}
