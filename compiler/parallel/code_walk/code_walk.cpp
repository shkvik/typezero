#include "compiler/parallel/code_walk/code_walk.h"
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

CodeWalk::CodeWalk(Analysis &analysis)
    : analysis(analysis) {}

int CodeWalk::discover(const FunctionDecl *fn) {
  if (this->analysis.byFunction[fn->id] != nullptr) {
    return fn->id;
  }
  FnInfo &info = this->analysis.records.emplace_back();
  this->analysis.byFunction[fn->id] = &info;

  info.decl = fn;
  info.async = fn->isAsync && !fn->isGenerator;
  info.asyncGen = fn->isAsync && fn->isGenerator;
  info.syncGen = !fn->isAsync && fn->isGenerator;
  info.thisOwner = this->analysis.thisOwnerOf(fn);

  if (fn->cls()) {
    noteClass(fn->cls());
  }
  info.segs.emplace_back();
  info.segs[0].loc = fn->loc();
  this->analysis.worklist.push_back(fn->id);

  return fn->id;
}

void CodeWalk::noteClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    if (this->analysis.classSeen.insert(cls).second) {
      this->analysis.classes.push_back(cls);
    }
  }
}

void CodeWalk::walk(int fi) {
  this->analysis.fi = fi;
  this->analysis.fn = this->analysis.byFunction[fi];
  this->analysis.cur = {0};
  this->analysis.targets.clear();
  this->analysis.openJoins.clear();
  this->analysis.loops = 0;
  this->analysis.label = {};
  this->analysis.anchors.clear();
  const FunctionDecl *decl = this->analysis.fn->decl;
  if (decl == this->analysis.module) {
    for (const Stmt *item : this->analysis.program.items) {
      if (!isa<FunctionDecl>(item) && !this->analysis.libraryEffects.inLibrary(item->loc())) {
        anchored(item);
      }
    }
    return;
  }
  for (const Param &p : decl->params) {
    if (p.defaultValue()) {
      this->analysis.exprWalk.expr(p.defaultValue());
    }
  }
  stmt(decl->body);
}

unsigned CodeWalk::segment(const Node *at, SegKind kind, llvm::SMLoc loc) {
  auto [it, inserted] =
      this->analysis.fn->segOf.try_emplace(at, unsigned(this->analysis.fn->segs.size()));
  if (inserted) {
    Segment &seg = this->analysis.fn->segs.emplace_back();
    seg.kind = kind;
    seg.at = at;
    seg.loc = loc;
  }

  return it->second;
}

CallRec &CodeWalk::record(const void *key, CallKind kind, const Use &use, llvm::SMLoc loc) {
  auto [it, inserted] = this->analysis.fn->callOf.try_emplace(
      std::make_pair(key, unsigned(kind)), unsigned(this->analysis.fn->calls.size()));
  if (inserted) {
    CallRec &rec = this->analysis.fn->calls.emplace_back();
    rec.function = this->analysis.fi;
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
  CallRec &rec = this->analysis.fn->calls[it->second];
  rec.repeated |= this->analysis.loops > 0;
  rec.inLoop |= this->analysis.loops > 0;
  ParallelModel::merge(rec.segs, this->analysis.cur);

  return rec;
}

void CodeWalk::touch(LocKind kind, Ref ref, const void *key, bool write, llvm::SMLoc at, Res res) {
  if (ref.kind == RefKind::Local || this->analysis.cur.empty()) {
    return;
  }
  Access a;
  a.loc.kind = kind;
  a.loc.ref = ref;
  a.loc.key = key;
  a.loc.res = res;
  a.write = write;
  a.at = at;
  if (!this->analysis.anchors.empty()) {
    a.anchor = this->analysis.anchors.back()->loc();
  }
  for (unsigned s : this->analysis.cur) {
    this->analysis.fn->segs[s].effects.insert(a);
  }
}

void CodeWalk::var(const VarSymbol *v, bool write, llvm::SMLoc at) {
  if (v && this->analysis.shared(v)) {
    touch(LocKind::Var, Ref(), v, write, at);
  }
}

void CodeWalk::joinPoint() {
  for (OpenJoin &j : this->analysis.openJoins) {
    ParallelModel::merge(j.active, this->analysis.cur);
  }
}

void CodeWalk::moveTo(unsigned seg) {
  for (const Stmt *anchor : this->analysis.anchors) {
    this->analysis.partedAnchors.insert(anchor->loc().getPointer());
  }
  for (unsigned s : this->analysis.cur) {
    this->analysis.fn->succ.insert({s, seg});
  }
  this->analysis.cur = {seg};
  joinPoint();
}

void CodeWalk::loop(llvm::StringRef label, llvm::function_ref<void()> head,
                    llvm::function_ref<void()> body, llvm::function_ref<void()> step,
                    bool headFirst) {
  Active entry = this->analysis.cur, exits;
  ++this->analysis.loops;
  while (true) {
    this->analysis.cur = entry;
    if (headFirst) {
      head();
      ParallelModel::merge(exits, this->analysis.cur);
    }
    this->analysis.targets.push_back({label, true, false, {}, {}});
    body();
    Target target = std::move(this->analysis.targets.back());
    this->analysis.targets.pop_back();
    ParallelModel::merge(this->analysis.cur, target.continues);
    if (step) {
      step();
    }
    if (!headFirst) {
      head();
      ParallelModel::merge(exits, this->analysis.cur);
    }
    ParallelModel::merge(exits, target.breaks);
    Active next = entry;
    ParallelModel::merge(next, this->analysis.cur);
    if (next == entry) {
      break;
    }
    entry = std::move(next);
  }
  --this->analysis.loops;
  this->analysis.cur = std::move(exits);
}

void CodeWalk::anchored(const Stmt *s) {
  if (!s) {
    return;
  }
  this->analysis.anchors.push_back(s);
  stmt(s);
  this->analysis.anchors.pop_back();
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
    this->analysis.exprWalk.expr(cast<ExprStmt>(s)->expr);
    return;
  case Stmt::Kind::If: {
    auto *i = cast<IfStmt>(s);
    this->analysis.exprWalk.expr(i->cond);
    Active before = this->analysis.cur;
    stmt(i->thenStmt);
    Active then = std::move(this->analysis.cur);
    this->analysis.cur = std::move(before);
    stmt(i->elseStmt);
    ParallelModel::merge(this->analysis.cur, then);
    return;
  }
  case Stmt::Kind::While: {
    auto *w = cast<WhileStmt>(s);
    loop(
        std::exchange(this->analysis.label, {}), [&] { this->analysis.exprWalk.expr(w->cond); },
        [&] { stmt(w->body); }, {}, true);
    return;
  }
  case Stmt::Kind::DoWhile: {
    auto *w = cast<DoWhileStmt>(s);
    loop(
        std::exchange(this->analysis.label, {}), [&] { this->analysis.exprWalk.expr(w->cond); },
        [&] { stmt(w->body); }, {}, false);
    return;
  }
  case Stmt::Kind::For: {
    auto *f = cast<ForStmt>(s);
    llvm::StringRef label = std::exchange(this->analysis.label, {});
    stmt(f->init);
    loop(
        label, [&] { this->analysis.exprWalk.expr(f->cond); }, [&] { stmt(f->body); },
        [&] { this->analysis.exprWalk.expr(f->step); }, true);
    return;
  }
  case Stmt::Kind::ForOf:
    forOf(cast<ForOfStmt>(s), std::exchange(this->analysis.label, {}));
    return;
  case Stmt::Kind::Switch: {
    auto *sw = cast<SwitchStmt>(s);
    llvm::StringRef label = std::exchange(this->analysis.label, {});
    this->analysis.exprWalk.expr(sw->discriminant);
    Active disc = this->analysis.cur, fall;
    bool hasDefault = false;
    this->analysis.targets.push_back({label, false, false, {}, {}});
    for (const SwitchCase &c : sw->cases) {
      this->analysis.cur = disc;
      ParallelModel::merge(this->analysis.cur, fall);
      if (c.test) {
        this->analysis.exprWalk.expr(c.test);
      } else {
        hasDefault = true;
      }
      for (const Stmt *x : c.body) {
        anchored(x);
      }
      fall = this->analysis.cur;
    }
    Target target = std::move(this->analysis.targets.back());
    this->analysis.targets.pop_back();
    this->analysis.cur = std::move(fall);
    ParallelModel::merge(this->analysis.cur, target.breaks);
    if (!hasDefault && !sw->exhaustive) {
      ParallelModel::merge(this->analysis.cur, disc);
    }
    return;
  }
  case Stmt::Kind::Labeled: {
    auto *l = cast<LabeledStmt>(s);
    if (isa<WhileStmt, DoWhileStmt, ForStmt, ForOfStmt, SwitchStmt>(l->body)) {
      this->analysis.label = l->label;
      stmt(l->body);
      return;
    }
    this->analysis.targets.push_back({l->label, false, true, {}, {}});
    stmt(l->body);
    Target target = std::move(this->analysis.targets.back());
    this->analysis.targets.pop_back();
    ParallelModel::merge(this->analysis.cur, target.breaks);
    return;
  }
  case Stmt::Kind::Return: {
    auto *r = cast<ReturnStmt>(s);
    Use use;
    use.returned = this->analysis.fn->async;
    if (!this->analysis.fn->async) {
      use.join = this->analysis.joinReturns.lookup(this->analysis.fn->decl);
      use.tail = !this->analysis.fn->syncGen && !this->analysis.fn->asyncGen;
    }
    this->analysis.exprWalk.expr(r->value, use);
    this->analysis.cur.clear();
    return;
  }
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue: {
    bool isBreak = isa<BreakStmt>(s);
    llvm::StringRef label = isBreak ? cast<BreakStmt>(s)->label : cast<ContinueStmt>(s)->label;
    for (auto it = this->analysis.targets.rbegin(); it != this->analysis.targets.rend(); ++it) {
      bool matches = !label.empty() ? it->label == label : isBreak ? !it->block : it->loop;
      if (!matches) {
        continue;
      }
      ParallelModel::merge(isBreak ? it->breaks : it->continues, this->analysis.cur);
      break;
    }
    this->analysis.cur.clear();
    return;
  }
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(s);
    if (cls->builtin() || cls->generic()) {
      return;
    }
    for (const ClassField &f : cls->fields) {
      if (f.isStatic() && f.init) {
        this->analysis.exprWalk.expr(f.init);
      }
    }
    for (const FunctionDecl *method : cls->methods) {
      if (method->impl()) {
        this->analysis.callWalk.registerValue(method, method->impl()->type);
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
  this->analysis.exprWalk.expr(d.init, use);
  if (!d.sym) {
    return;
  }
  const Expr *init = ParallelHelpers::strip(d.init);
  this->analysis.declInit[d.sym] = init;
  if (!d.sym->reassigned && this->analysis.aliasAnalysis.isAllocation(init)) {
    this->analysis.siteName.try_emplace(init, d.name);
  }
  if (d.sym->capturedEarly()) {
    var(d.sym, true, d.loc);
  }
}

void CodeWalk::forOf(const ForOfStmt *f, llvm::StringRef label) {
  this->analysis.exprWalk.expr(f->iterable);
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
  const Expr *source = ParallelHelpers::strip(iterable);
  if (type->runtimeKind() == Type::Runtime::Readline ||
      type->runtimeKind() == Type::Runtime::ReadlinePromises) {
    this->analysis.resource(Res::Stdin, true, at);
    return;
  }
  if (type->isAsyncIterator() || type->isIterator()) {
    bool asyncIter = type->isAsyncIterator();
    CallRec &rec = record(site, asyncIter ? CallKind::IterAsync : CallKind::IterSync, {}, at);
    auto *c = dyn_cast<CallExpr>(source);
    if (c && this->analysis.libraryEffects.libraryTarget(c)) {
      const FunctionDecl *lib = this->analysis.libraryEffects.libraryTarget(c);
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (m && c->method()) {
        this->analysis.libraryEffects.libraryState(
            ParallelHelpers::libraryClassName(lib->cls()), lib->name,
            this->analysis.aliasAnalysis.receiverRef(m->object), true, at);
      }
      return;
    }
    if (c && (c->func() || c->method())) {
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (c->method()) {
        for (const FunctionDecl *fn :
             this->analysis.callWalk.methodTargets(c->method()->decl, c->virtualCall())) {
          this->analysis.callWalk.addCallee(
              rec, fn,
              m ? std::optional<Ref>(this->analysis.aliasAnalysis.receiverRef(m->object))
                : std::nullopt);
        }
      } else {
        this->analysis.callWalk.addCallee(
            rec, c->func()->decl,
            this->analysis.callWalk.closureSelf(c->func()->decl, *this->analysis.fn));
      }
      return;
    }
    if (c && c->builtin() != Builtin::None && ParallelHelpers::makesNewObject(c->builtin())) {
      return;
    }
    rec.byType = type;
    this->analysis.exprWalk.elements(iterable, true, at);
    return;
  }
  if (type->isArray() || type->isDict() || type->isTypedArray() || type->isJson()) {
    this->analysis.exprWalk.elements(iterable, false, at);
  }
}

}
