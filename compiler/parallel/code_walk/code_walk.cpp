#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
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

CodeWalk::CodeWalk(Analysis &analysis) : analysis_(analysis) {}

unsigned CodeWalk::discover(const FunctionDecl *fn) {
  auto [it, inserted] = analysis_.fnIndex_.try_emplace(fn, unsigned(analysis_.fns_.size()));
  if (!inserted) {
    return it->second;
  }
  FnInfo &info = analysis_.fns_.emplace_back();
  info.decl = fn;
  if (fn) {
    info.async = fn->isAsync && !fn->isGenerator;
    info.asyncGen = fn->isAsync && fn->isGenerator;
    info.syncGen = !fn->isAsync && fn->isGenerator;
    info.thisOwner = analysis_.thisOwnerOf(fn);
    if (fn->cls) {
      noteClass(fn->cls);
    }
  }
  info.segs.emplace_back();
  if (fn) {
    info.segs[0].loc = fn->loc();
  }
  analysis_.worklist_.push_back(it->second);

  return it->second;
}

void CodeWalk::noteClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    if (analysis_.classSeen_.insert(cls).second) {
      analysis_.classes_.push_back(cls);
    }
  }
}

void CodeWalk::walk(unsigned fi) {
  analysis_.fi_ = fi;
  analysis_.fn_ = &analysis_.fns_[fi];
  analysis_.cur_ = {0};
  analysis_.targets_.clear();
  analysis_.tries_.clear();
  analysis_.openJoins_.clear();
  analysis_.loops_ = 0;
  analysis_.label_ = {};
  const FunctionDecl *decl = analysis_.fn_->decl;
  if (!decl) {
    for (const Stmt *item : analysis_.program_.items) {
      if (!isa<FunctionDecl>(item) && !analysis_.libraryEffects().inLibrary(item->loc())) {
        stmt(item);
      }
    }
    return;
  }
  for (const Param &p : decl->params) {
    if (p.defaultValue) {
      analysis_.exprWalk().expr(p.defaultValue);
    }
  }
  stmt(decl->body);
}

unsigned CodeWalk::segment(const Node *at, SegKind kind, llvm::SMLoc loc) {
  auto [it, inserted] = analysis_.fn_->segOf.try_emplace(at, unsigned(analysis_.fn_->segs.size()));
  if (inserted) {
    Segment &seg = analysis_.fn_->segs.emplace_back();
    seg.kind = kind;
    seg.at = at;
    seg.loc = loc;
  }

  return it->second;
}

CallRec &CodeWalk::record(const void *key, CallKind kind, const Use &use, llvm::SMLoc loc) {
  auto [it, inserted] = analysis_.fn_->callOf.try_emplace(std::make_pair(key, unsigned(kind)),
                                                          unsigned(analysis_.fn_->calls.size()));
  if (inserted) {
    CallRec &rec = analysis_.fn_->calls.emplace_back();
    rec.kind = kind;
    rec.site = static_cast<const Node *>(key);
    rec.loc = loc;
    rec.await = use.awaited;
    rec.returned = use.returned;
    rec.voided = use.voided;
    rec.join = use.join;
  }
  CallRec &rec = analysis_.fn_->calls[it->second];
  rec.repeated |= analysis_.loops_ > 0;
  rec.inLoop |= analysis_.loops_ > 0;
  merge(rec.segs, analysis_.cur_);

  return rec;
}

void CodeWalk::touch(LocKind kind, Ref ref, const void *key, bool write, llvm::SMLoc at, Res res) {
  if (ref.kind == RefKind::Local || analysis_.cur_.empty()) {
    return;
  }
  Access a;
  a.loc.kind = kind;
  a.loc.ref = ref;
  a.loc.key = key;
  a.loc.res = res;
  a.write = write;
  a.at = at;
  for (unsigned s : analysis_.cur_) {
    analysis_.fn_->segs[s].effects.insert(a);
  }
}

void CodeWalk::var(const VarSymbol *v, bool write, llvm::SMLoc at) {
  if (v && analysis_.shared(v)) {
    touch(LocKind::Var, Ref(), v, write, at);
  }
}

// The pieces active now may throw, into the nearest `catch` or `finally`.
void CodeWalk::throwPoint() {
  for (auto it = analysis_.tries_.rbegin(); it != analysis_.tries_.rend(); ++it) {
    if (it->phase == Analysis::Phase::Block) {
      merge(it->entry, analysis_.cur_);
      merge(it->finallyEntry, analysis_.cur_);
      if (it->handler) {
        return;
      }
    } else if (it->phase == Analysis::Phase::Handler) {
      merge(it->finallyEntry, analysis_.cur_);
    }
  }
}

// A return, break or continue runs the `finally` blocks it leaves.
void CodeWalk::throughFinally() {
  for (Analysis::TryState &t : analysis_.tries_) {
    if (t.finalizer && t.phase != Analysis::Phase::Finalizer) {
      merge(t.finallyEntry, analysis_.cur_);
    }
  }
}

// Whether an error here could be caught in this function, or run a
// `finally` block, before it leaves the function.
bool CodeWalk::caught() const {
  for (auto it = analysis_.tries_.rbegin(); it != analysis_.tries_.rend(); ++it) {
    if (it->phase == Analysis::Phase::Block && (it->handler || it->finalizer)) {
      return true;
    }
    if (it->phase == Analysis::Phase::Handler && it->finalizer) {
      return true;
    }
  }

  return false;
}

void CodeWalk::joinPoint() {
  for (Analysis::OpenJoin &j : analysis_.openJoins_) {
    merge(j.active, analysis_.cur_);
  }
}

// The code waits here and goes on in segment `seg`.
void CodeWalk::moveTo(unsigned seg) {
  for (unsigned s : analysis_.cur_) {
    analysis_.fn_->succ.insert({s, seg});
  }
  analysis_.cur_ = {seg};
  joinPoint();
  throwPoint();
}

// A loop: `head` runs before each pass (the condition, or the next step of an
// iteration) and the loop may end after it; then the pass, `body`, and
// `step`, where `continue` goes too. A do-while has its head after the body.
// The passes are walked until the pieces active at the head settle.
void CodeWalk::loop(llvm::StringRef label, llvm::function_ref<void()> head,
                    llvm::function_ref<void()> body, llvm::function_ref<void()> step,
                    bool headFirst) {
  Active entry = analysis_.cur_, exits;
  ++analysis_.loops_;
  while (true) {
    analysis_.cur_ = entry;
    if (headFirst) {
      head();
      merge(exits, analysis_.cur_);
    }
    analysis_.targets_.push_back({label, true, false, {}, {}});
    body();
    Analysis::Target target = std::move(analysis_.targets_.back());
    analysis_.targets_.pop_back();
    merge(analysis_.cur_, target.continues);
    if (step) {
      step();
    }
    if (!headFirst) {
      head();
      merge(exits, analysis_.cur_);
    }
    merge(exits, target.breaks);
    Active next = entry;
    merge(next, analysis_.cur_);
    if (next == entry) {
      break;
    }
    entry = std::move(next);
  }
  --analysis_.loops_;
  analysis_.cur_ = std::move(exits);
}

void CodeWalk::stmt(const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *x : cast<BlockStmt>(s)->body) {
      stmt(x);
    }
    return;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls) {
      declarator(d);
    }
    return;
  case Stmt::Kind::Function:
    // Declared here; it runs when something calls it.
    return;
  case Stmt::Kind::Expr:
    analysis_.exprWalk().expr(cast<ExprStmt>(s)->expr);
    return;
  case Stmt::Kind::If: {
    auto *i = cast<IfStmt>(s);
    analysis_.exprWalk().expr(i->cond);
    Active before = analysis_.cur_;
    stmt(i->thenStmt);
    Active then = std::move(analysis_.cur_);
    analysis_.cur_ = std::move(before);
    stmt(i->elseStmt);
    merge(analysis_.cur_, then);
    return;
  }
  case Stmt::Kind::While: {
    auto *w = cast<WhileStmt>(s);
    loop(
        std::exchange(analysis_.label_, {}), [&] { analysis_.exprWalk().expr(w->cond); },
        [&] { stmt(w->body); }, {}, true);
    return;
  }
  case Stmt::Kind::DoWhile: {
    auto *w = cast<DoWhileStmt>(s);
    loop(
        std::exchange(analysis_.label_, {}), [&] { analysis_.exprWalk().expr(w->cond); },
        [&] { stmt(w->body); }, {}, false);
    return;
  }
  case Stmt::Kind::For: {
    auto *f = cast<ForStmt>(s);
    llvm::StringRef label = std::exchange(analysis_.label_, {});
    stmt(f->init);
    loop(
        label, [&] { analysis_.exprWalk().expr(f->cond); }, [&] { stmt(f->body); },
        [&] { analysis_.exprWalk().expr(f->step); }, true);
    return;
  }
  case Stmt::Kind::ForOf:
    forOf(cast<ForOfStmt>(s), std::exchange(analysis_.label_, {}));
    return;
  case Stmt::Kind::Switch: {
    auto *sw = cast<SwitchStmt>(s);
    llvm::StringRef label = std::exchange(analysis_.label_, {});
    analysis_.exprWalk().expr(sw->discriminant);
    Active disc = analysis_.cur_, fall;
    bool hasDefault = false;
    analysis_.targets_.push_back({label, false, false, {}, {}});
    for (const SwitchCase &c : sw->cases) {
      analysis_.cur_ = disc;
      merge(analysis_.cur_, fall);
      if (c.test) {
        analysis_.exprWalk().expr(c.test);
      } else {
        hasDefault = true;
      }
      for (const Stmt *x : c.body) {
        stmt(x);
      }
      fall = analysis_.cur_;
    }
    Analysis::Target target = std::move(analysis_.targets_.back());
    analysis_.targets_.pop_back();
    analysis_.cur_ = std::move(fall);
    merge(analysis_.cur_, target.breaks);
    if (!hasDefault && !sw->exhaustive) {
      merge(analysis_.cur_, disc);
    }
    return;
  }
  case Stmt::Kind::Labeled: {
    auto *l = cast<LabeledStmt>(s);
    if (isa<WhileStmt, DoWhileStmt, ForStmt, ForOfStmt, SwitchStmt>(l->body)) {
      analysis_.label_ = l->label;
      stmt(l->body);
      return;
    }
    analysis_.targets_.push_back({l->label, false, true, {}, {}});
    stmt(l->body);
    Analysis::Target target = std::move(analysis_.targets_.back());
    analysis_.targets_.pop_back();
    merge(analysis_.cur_, target.breaks);
    return;
  }
  case Stmt::Kind::Return: {
    auto *r = cast<ReturnStmt>(s);
    Use use;
    use.returned = analysis_.fn_->async;
    // A callback of map whose results Promise.all waits for.
    if (!analysis_.fn_->async && analysis_.fn_->decl) {
      use.join = analysis_.joinReturns_.lookup(analysis_.fn_->decl);
    }
    analysis_.exprWalk().expr(r->value, use);
    throughFinally();
    analysis_.cur_.clear();
    return;
  }
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue: {
    bool isBreak = isa<BreakStmt>(s);
    llvm::StringRef label = isBreak ? cast<BreakStmt>(s)->label : cast<ContinueStmt>(s)->label;
    for (auto it = analysis_.targets_.rbegin(); it != analysis_.targets_.rend(); ++it) {
      // Without a label, break leaves the innermost loop or switch, and
      // continue goes on with the innermost loop.
      bool matches = !label.empty() ? it->label == label : isBreak ? !it->block : it->loop;
      if (!matches) {
        continue;
      }
      throughFinally();
      merge(isBreak ? it->breaks : it->continues, analysis_.cur_);
      break;
    }
    analysis_.cur_.clear();
    return;
  }
  case Stmt::Kind::Throw:
    analysis_.exprWalk().expr(cast<ThrowStmt>(s)->value);
    throwPoint();
    analysis_.cur_.clear();
    return;
  case Stmt::Kind::Try: {
    auto *t = cast<TryStmt>(s);
    analysis_.tries_.push_back(
        {Analysis::Phase::Block, t->handler != nullptr, t->finalizer != nullptr, {}, {}});
    stmt(t->block);
    merge(analysis_.tries_.back().finallyEntry, analysis_.cur_);
    Active normal = analysis_.cur_;
    if (t->handler) {
      analysis_.tries_.back().phase = Analysis::Phase::Handler;
      analysis_.cur_ = analysis_.tries_.back().entry;
      stmt(t->handler);
      merge(analysis_.tries_.back().finallyEntry, analysis_.cur_);
      merge(normal, analysis_.cur_);
    }
    Analysis::TryState done = std::move(analysis_.tries_.back());
    analysis_.tries_.pop_back();
    if (t->finalizer) {
      // The block runs on every way out of the try; after it the code goes
      // on from any piece it may end in.
      analysis_.cur_ = std::move(done.finallyEntry);
      stmt(t->finalizer);
    } else {
      analysis_.cur_ = std::move(normal);
    }
    return;
  }
  case Stmt::Kind::Class: {
    // The static fields get their values and the decorators run where the
    // class is declared; its methods run when called.
    auto *cls = cast<ClassDecl>(s);
    if (cls->builtin || cls->generic) {
      return;
    }
    for (const ClassField &f : cls->fields) {
      if (f.isStatic && f.init) {
        analysis_.exprWalk().expr(f.init);
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
  if (d.sym && d.sym->lambda) {
    use.lambda = true; // calls through the constant go to the arrow directly
  }
  analysis_.exprWalk().expr(d.init, use);
  if (!d.sym) {
    return;
  }
  const Expr *init = strip(d.init);
  analysis_.declInit_[d.sym] = init;
  if (!d.sym->reassigned && analysis_.aliasAnalysis().isAllocation(init)) {
    analysis_.siteName_.try_emplace(init, d.name);
  }
  // A closure made before the declaration may see the variable change here.
  if (d.sym->capturedEarly) {
    var(d.sym, true, d.loc);
  }
}

void CodeWalk::forOf(const ForOfStmt *f, llvm::StringRef label) {
  analysis_.exprWalk().expr(f->iterable);
  loop(
      label,
      [&] {
        iterate(f->iterable, f);
        if (f->isAwait) {
          moveTo(segment(f, SegKind::ForAwait, f->loc())); // each step waits for the next value
        }
      },
      [&] { stmt(f->body); }, {}, true);
}

// One step of iterating `iterable`: an iterator moves on, and a generator
// runs up to its next value.
void CodeWalk::iterate(const Expr *iterable, const Node *site) {
  if (!iterable || !iterable->type) {
    return;
  }
  const Type *type = iterable->type->nonOptional();
  llvm::SMLoc at = iterable->loc();
  const Expr *source = strip(iterable);
  // The lines of readline: each step takes the next line of input.
  if (type->runtimeKind() == Type::Runtime::Readline ||
      type->runtimeKind() == Type::Runtime::ReadlinePromises) {
    analysis_.resource(Res::Stdin, true, at);
    return;
  }
  if (type->isAsyncIterator() || type->isIterator()) {
    throwPoint(); // a generator may throw as it runs
    bool asyncIter = type->isAsyncIterator();
    CallRec &rec = record(site, asyncIter ? CallKind::IterAsync : CallKind::IterSync, {}, at);
    auto *c = dyn_cast<CallExpr>(source);
    if (c && analysis_.libraryEffects().libraryTarget(c)) {
      // An iterator of a built-in module (a stream, on() of node:events):
      // each step moves on what it iterates, and the listeners and sources
      // the program gave run as their calls say.
      const FunctionDecl *lib = analysis_.libraryEffects().libraryTarget(c);
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (m && c->method) {
        analysis_.libraryEffects().libraryState(libraryClassName(lib->cls), lib->name,
                                                analysis_.aliasAnalysis().receiverRef(m->object),
                                                true, at);
      }
      return;
    }
    if (c && (c->func || c->method)) {
      // A generator called right here: its iterator is no one else's.
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (c->method) {
        for (const FunctionDecl *fn :
             analysis_.callWalk().methodTargets(c->method->decl, c->virtualCall)) {
          analysis_.callWalk().addCallee(
              rec, fn,
              m ? std::optional<Ref>(analysis_.aliasAnalysis().receiverRef(m->object))
                : std::nullopt);
        }
      } else {
        analysis_.callWalk().addCallee(
            rec, c->func->decl, analysis_.callWalk().closureSelf(c->func->decl, *analysis_.fn_));
      }
      return;
    }
    // An iterator a built-in makes right here (of an array, a map...) runs
    // no code of the program, and is no one else's.
    if (c && c->builtin != Builtin::None && makesNewObject(c->builtin)) {
      return;
    }
    rec.byType = type;
    analysis_.exprWalk().elements(iterable, true, at);
    return;
  }
  if (type->isArray() || type->isDict() || type->isTypedArray() || type->isJson()) {
    analysis_.exprWalk().elements(iterable, false, at);
  }
}

} // namespace tz::parallel
