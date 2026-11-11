#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr double kUnrollPasses = 24;

constexpr const char *kWhileItRuns =
    ", and nothing in the code limits it: while it runs without 'await', nothing else runs on "
    "this thread";

constexpr size_t kRememberedPerLoop = 16;

}

LoopRanges::LoopRanges(OverflowCheck &check)
    : check(check) {}

bool LoopRanges::yields(const Stmt *stmt) const {
  for (const Expr *e : AstWalker::exprsIn(stmt, false)) {
    if (isa<YieldExpr>(e)) {
      return true;
    }
  }

  return false;
}

bool LoopRanges::openEnded(const Expr *cond, const Stmt *body) const {
  if (this->yields(body)) {
    return false;
  }
  if (!cond) {
    return true;
  }
  if (auto *lit = dyn_cast<BoolLitExpr>(OverflowHelpers::stripCasts(cond))) {
    return lit->value;
  }

  return this->readsInput(cond);
}

bool LoopRanges::grows(const Stmt *body, const VarSymbol *var) const {
  auto isVar = [&](const Expr *e) {
    auto *name = dyn_cast_or_null<NameExpr>(OverflowHelpers::stripCasts(e));

    return name && name->var() == var;
  };
  for (const Expr *e : AstWalker::exprsIn(body, true)) {
    if (auto *call = dyn_cast<CallExpr>(e)) {
      auto *member = dyn_cast<MemberExpr>(call->callee);
      if (member && isVar(member->object) &&
          (call->builtin() == Builtin::ArrPush || call->builtin() == Builtin::ArrUnshift ||
           call->builtin() == Builtin::ArrSplice)) {
        return true;
      }
    } else if (auto *assign = dyn_cast<AssignExpr>(e)) {
      if (auto *index = dyn_cast<IndexExpr>(assign->target);
          index && isVar(index->object) && index->mayAppend()) {
        return true;
      }
      if (auto *member = dyn_cast<MemberExpr>(assign->target);
          member && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) &&
          isVar(member->object)) {
        return true;
      }
    }
  }

  return false;
}

bool LoopRanges::identicalNum(const Num &a, const Num &b) const {
  return (a.lo == b.lo || (std::isnan(a.lo) && std::isnan(b.lo))) &&
         (a.hi == b.hi || (std::isnan(a.hi) && std::isnan(b.hi))) && a.loOrigin == b.loOrigin &&
         a.hiOrigin == b.hiOrigin && a.loWhy == b.loWhy && a.hiWhy == b.hiWhy && a.base == b.base &&
         a.offLo == b.offLo && a.offHi == b.offHi && a.away == b.away;
}

bool LoopRanges::identicalVal(const Val &a, const Val &b) const {
  const Val *left = &a;
  const Val *right = &b;
  while (this->identicalNum(left->num, right->num) && this->identicalNum(left->len, right->len)) {
    if (left->elem == right->elem) {
      return true;
    }
    if (left->elem == nullptr || right->elem == nullptr) {
      return false;
    }
    left = left->elem.get();
    right = right->elem.get();
  }

  return false;
}

bool LoopRanges::identicalState(const State &a, const State &b) const {
  if (a.dead != b.dead || a.vars.size() != b.vars.size()) {
    return false;
  }
  for (const auto &entry : a.vars) {
    auto found = b.vars.find(entry.first);
    if (found == b.vars.end() || !this->identicalVal(entry.second, found->second)) {
      return false;
    }
  }

  return true;
}

bool LoopRanges::awaitsIn(const Stmt *stmt) const {
  auto pending = std::vector<const Stmt *>{stmt};
  while (!pending.empty()) {
    const Stmt *next = pending.back();
    pending.pop_back();
    if (next == nullptr) {
      continue;
    }
    const auto *forOf = dyn_cast<ForOfStmt>(next);
    if (forOf != nullptr && forOf->isAwait) {
      return true;
    }
    auto parts = this->awaitParts(next);
    pending.insert(pending.end(), parts.rbegin(), parts.rend());
  }

  return false;
}

std::vector<const Stmt *> LoopRanges::awaitParts(const Stmt *stmt) const {
  switch (stmt->kind()) {
  case Stmt::Kind::ForOf:
    return {cast<ForOfStmt>(stmt)->body};
  case Stmt::Kind::While:
    return {cast<WhileStmt>(stmt)->body};
  case Stmt::Kind::DoWhile:
    return {cast<DoWhileStmt>(stmt)->body};
  case Stmt::Kind::For:
    return {cast<ForStmt>(stmt)->body};
  case Stmt::Kind::Block: {
    const auto &body = cast<BlockStmt>(stmt)->body;
    return std::vector<const Stmt *>(body.begin(), body.end());
  }
  case Stmt::Kind::If: {
    const auto *branch = cast<IfStmt>(stmt);
    return {branch->thenStmt, branch->elseStmt};
  }
  case Stmt::Kind::Labeled:
    return {cast<LabeledStmt>(stmt)->body};
  case Stmt::Kind::Switch: {
    auto parts = std::vector<const Stmt *>();
    for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
      parts.insert(parts.end(), c.body.begin(), c.body.end());
    }
    return parts;
  }
  default:
    return {};
  }
}

bool LoopRanges::waitsOrYields(const Stmt *stmt) const {
  for (const Expr *e : AstWalker::exprsIn(stmt, false)) {
    if (isa<AwaitExpr, YieldExpr>(e)) {
      return true;
    }
  }

  return this->awaitsIn(stmt);
}

bool LoopRanges::readsInput(const Expr *cond) const {
  if (cond == nullptr) {
    return false;
  }
  for (const Expr *e : AstWalker::exprsIn(cond, true)) {
    auto *call = dyn_cast<CallExpr>(e);
    if (call == nullptr) {
      continue;
    }
    bool isInput = call->builtin() == Builtin::ReadLine ||
                   BuiltinGroups::isFsBuiltin(call->builtin()) ||
                   BuiltinGroups::isChildBuiltin(call->builtin());
    if (isInput) {
      return true;
    }
  }

  return false;
}

std::optional<double> LoopRanges::counterStep(const Expr *arg, const VarSymbol *param) const {
  auto *bin = dyn_cast<BinaryExpr>(OverflowHelpers::stripCasts(arg));
  if (!bin || (bin->op != BinaryOp::Add && bin->op != BinaryOp::Sub)) {
    return std::nullopt;
  }
  auto isParam = [&](const Expr *e) {
    auto *name = dyn_cast<NameExpr>(OverflowHelpers::stripCasts(e));

    return name && name->var() == param;
  };
  auto literal = [](const Expr *e) -> std::optional<double> {
    if (auto *lit = dyn_cast<IntLitExpr>(OverflowHelpers::stripCasts(e)); lit && lit->value > 0) {
      return double(lit->value);
    }

    return std::nullopt;
  };
  if (isParam(bin->lhs)) {
    if (std::optional<double> c = literal(bin->rhs)) {
      return bin->op == BinaryOp::Add ? *c : -*c;
    }
  }
  if (bin->op == BinaryOp::Add && isParam(bin->rhs)) {
    return literal(bin->lhs);
  }

  return std::nullopt;
}

bool LoopRanges::limitedTowards(const Num &n, double step) const {
  if (n.empty()) {
    return false;
  }

  return step < 0 ? n.loOrigin == Origin::Proven && n.lo > -kLengthMax
                  : n.hiOrigin == Origin::Proven && n.hi < kLengthMax;
}

bool LoopRanges::freshBase(const Val &v, uint64_t before) const {
  for (const Val *link = &v; link != nullptr; link = link->elem.get()) {
    if (link->num.base > before || link->len.base > before) {
      return true;
    }
  }

  return false;
}

std::optional<std::pair<double, double>> LoopRanges::stepOf(const Num &end, uint64_t base) const {
  if (end.empty() || end.base != base || !base) {
    return std::nullopt;
  }

  return std::make_pair(end.offLo, end.offHi);
}

void LoopRanges::execLoop(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels, State &st) {
  OverflowCheck::LoopSpec spec;
  spec.stmt = stmt;
  spec.loc = stmt->loc();
  spec.labels.assign(labels.begin(), labels.end());
  if (auto *s = dyn_cast<WhileStmt>(stmt)) {
    spec.cond = s->cond;
    spec.openEnded = this->openEnded(s->cond, s->body);
    spec.simple = !OverflowHelpers::containsLoop(s->body);
    spec.body = [this, s](State &in) { this->check.executor().exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  if (auto *s = dyn_cast<DoWhileStmt>(stmt)) {
    spec.condAfter = s->cond;
    spec.openEnded = this->openEnded(s->cond, s->body);
    spec.simple = !OverflowHelpers::containsLoop(s->body);
    spec.body = [this, s](State &in) { this->check.executor().exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  if (auto *s = dyn_cast<ForStmt>(stmt)) {
    this->check.executor().exec(s->init, st);
    spec.cond = s->cond;
    spec.step = s->step;
    spec.openEnded = this->openEnded(s->cond, s->body);
    spec.simple = !OverflowHelpers::containsLoop(s->body);
    spec.body = [this, s](State &in) { this->check.executor().exec(s->body, in); };
    runLoop(spec, st);
    return;
  }
  auto *s = cast<ForOfStmt>(stmt);
  Val iterable = this->check.evaluator().eval(s->iterable, st);
  const Type *type = s->iterable->type ? s->iterable->type->nonOptional() : nullptr;
  const Type *bound = s->binding.sym ? s->binding.sym->type : nullptr;
  auto *name = dyn_cast<NameExpr>(OverflowHelpers::stripCasts(s->iterable));
  const VarSymbol *iterated = name && st.vars.count(name->var()) ? name->var() : nullptr;
  Val element = this->check.overflowWarnings().trusted(bound);
  if (type && type->isArray()) {
    element = this->check.slots().elementsOf(s->iterable, iterable, type);
    if (!iterated || !this->grows(s->body, iterated)) {
      spec.passes = ValueRange::add(iterable.len, ValueRange::exactly(1));
    }
  } else if (type && type->isString()) {
    element = ValueRange::lengthVal(ValueRange::range(1, 2));
    spec.passes = ValueRange::add(iterable.len, ValueRange::exactly(1));
  } else if (type && type->isDict()) {
    spec.passes = ValueRange::someLength();
  }
  spec.source = iterable;
  spec.exitAtHead = true;
  spec.simple = !OverflowHelpers::containsLoop(s->body);
  spec.body = [this, s, type, element, iterated](State &in) {
    Val e = element;
    if (iterated && type && type->isArray()) {
      e = this->check.slots().elementsOf(s->iterable, this->check.slots().readVar(iterated, in),
                                         type);
    }
    this->check.slots().writeVar(s->binding.sym, e, in);
    this->check.executor().exec(s->body, in);
  };
  runLoop(spec, st);
}

State LoopRanges::onePass(const OverflowCheck::LoopSpec &spec, const State &head, State &exits) {
  ++this->check.work;
  State s = head;
  if (spec.exitAtHead) {
    exits = ValueRange::join(exits, s);
  }
  if (spec.cond) {
    State t, f;
    this->check.narrowing().refine(spec.cond, s, t, f);
    exits = ValueRange::join(exits, f);
    s = t;
  }
  OverflowCheck::Target target;
  target.loop = true;
  target.labels = spec.labels;
  target.quiet = this->check.quiet;
  this->check.targets.push_back(std::move(target));
  if (!s.dead) {
    spec.body(s);
  }
  State conts = this->check.targets.back().conts, breaks = this->check.targets.back().breaks;
  this->check.targets.pop_back();
  s = ValueRange::join(s, conts);
  exits = ValueRange::join(exits, breaks);
  if (spec.step && !s.dead) {
    this->check.evaluator().eval(spec.step, s);
  }
  if (spec.condAfter && !s.dead) {
    State t, f;
    this->check.narrowing().refine(spec.condAfter, s, t, f);
    exits = ValueRange::join(exits, f);
    s = t;
  }

  return s;
}

State LoopRanges::rebased(const State &state, llvm::DenseMap<const VarSymbol *, uint64_t> &bases) {
  State r = state;
  for (auto &entry : r.vars) {
    uint64_t &base = bases[entry.first];
    if (!base) {
      base = ++this->check.bases_;
      ++this->check.bases_;
    }
    entry.second.num.base = base;
    entry.second.num.offLo = entry.second.num.offHi = 0;
    entry.second.len.base = base + 1;
    entry.second.len.offLo = entry.second.len.offHi = 0;
  }

  return r;
}

Num LoopRanges::passCount(const OverflowCheck::LoopSpec &spec, const State &entry,
                          const State &head, const State &first, const State &firstEnd) {
  if (firstEnd.dead) {
    return spec.passes;
  }
  Num best = spec.passes;
  if (!best.empty()) {
    best = ValueRange::make(Bound{0}, Bound{std::max(best.hi, 0.0), best.hiOrigin, best.hiWhy});
  }
  auto consider = [&](double n, const Bound &limit) {
    if (!(n >= 0) || n == kInf) {
      return;
    }
    if (best.empty() || n < best.hi) {
      best = ValueRange::make(Bound{0}, Bound{std::floor(n), limit.origin, limit.why});
    }
  };
  for (const auto &entryVar : entry.vars) {
    const VarSymbol *var = entryVar.first;
    auto h = head.vars.find(var), f = first.vars.find(var), e = firstEnd.vars.find(var);
    if (h == head.vars.end() || f == first.vars.end() || e == firstEnd.vars.end()) {
      continue;
    }
    const Num &start = entryVar.second.num;
    if (std::optional<std::pair<double, double>> step =
            this->stepOf(e->second.num, f->second.num.base);
        step && step->first == step->second && step->first != 0 &&
        ValueRange::isIntegerType(var->type) && !start.empty()) {
      double s = step->first;
      Limits l = ValueRange::limitsOf(var->type);
      const Num &bounded = h->second.num;
      if (s > 0 && bounded.hi < l.hi - s) {
        consider((bounded.hi - start.lo) / s + 1, bounded.high());
      }
      if (s < 0 && bounded.lo > l.lo - s) {
        consider((start.hi - bounded.lo) / -s + 1, bounded.low());
      }
    }
    if (std::optional<std::pair<double, double>> step =
            this->stepOf(e->second.len, f->second.len.base);
        step && step->second < 0 && !entryVar.second.len.empty()) {
      consider(entryVar.second.len.hi / -step->second + 1, entryVar.second.len.high());
    }
  }

  return best;
}

void LoopRanges::forget() {
  this->remembered.clear();
}

void LoopRanges::noteLoop(const OverflowCheck::LoopSpec &spec, const Num &passes) {
  if (!spec.stmt || !this->check.reporting || this->check.quiet != 1 || this->check.overBudget()) {
    return;
  }
  OverflowCheck::LongLoop &loop = this->check.loops[spec.stmt];
  if (!passes.empty() && passes.hiOrigin == Origin::Proven && passes.hi < kLengthMax) {
    return;
  }
  loop.bounded = false;
  if (!loop.why && !passes.empty() && passes.hiOrigin == Origin::Input) {
    loop.why = passes.hiWhy;
  }
}

void LoopRanges::findRecursions() {
  for (const FunctionDecl *fn : this->check.functions_) {
    if (!fn->body || !fn->sym || !fn->sym->hasReturnType() || fn->isAsync || fn->isGenerator) {
      continue;
    }
    OverflowCheck::Recursion recursion;
    for (CallExpr *call : AstWalker::tailCallsIn(const_cast<FunctionDecl *>(fn))) {
      if (call->func() && call->func()->decl == fn &&
          call->func()->getKnownReturnType() == fn->sym->getKnownReturnType()) {
        recursion.self.push_back(call);
      }
    }
    if (recursion.self.empty()) {
      continue;
    }
    std::vector<const Expr *> exprs = AstWalker::exprsIn(fn->body, false);
    auto otherSelfCall = [&](const Expr *e) {
      auto *call = dyn_cast<CallExpr>(e);
      return call != nullptr && call->func() != nullptr && call->func()->decl == fn &&
             !llvm::is_contained(recursion.self, call);
    };
    if (llvm::any_of(exprs, otherSelfCall)) {
      continue;
    }
    for (size_t i = 0; i < fn->params.size() && recursion.counter < 0; ++i) {
      const VarSymbol *param = fn->params[i].sym;
      if (!param || !ValueRange::isIntegerType(param->type)) {
        continue;
      }
      std::optional<double> step;
      for (const CallExpr *call : recursion.self) {
        std::optional<double> s =
            i < call->args.size() ? this->counterStep(call->args[i], param) : std::nullopt;
        if (!s || (step && (*s < 0) != (*step < 0))) {
          step.reset();
          break;
        }
        step = s;
      }
      if (step) {
        recursion.counter = int(i);
        recursion.step = *step;
      }
    }
    this->check.recursions[fn] = std::move(recursion);
  }
}

void LoopRanges::noteCall(const CallExpr *call, const FunctionDecl *target,
                          llvm::ArrayRef<Val> args) {
  if (!this->check.reporting || this->check.quiet != 0 || this->check.overBudget() ||
      !call->func() || !this->check.recursions.count(target)) {
    return;
  }
  OverflowCheck::CallArgs &known = this->check.recursiveCalls[call];
  bool isFirst = !known.target;
  known.target = target;
  known.args.resize(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    known.args[i] = isFirst ? args[i].num : ValueRange::join(known.args[i], args[i].num);
  }
}

std::vector<Diagnostic> LoopRanges::findLongRecursions() {
  auto hints = std::vector<Diagnostic>();
  for (const auto &[fn, recursion] : this->check.recursions) {
    bool isStopping = recursion.counter >= 0;
    bool isSeen = true;
    for (const CallExpr *call : recursion.self) {
      auto found = this->check.recursiveCalls.find(call);
      if (found == this->check.recursiveCalls.end()) {
        isSeen = false;
        continue;
      }
      isStopping = isStopping && size_t(recursion.counter) < found->second.args.size() &&
                   this->limitedTowards(found->second.args[recursion.counter], recursion.step);
    }
    if (!isSeen) {
      continue;
    }
    std::string name = fn->name.empty() ? std::string("this function") : "'" + fn->name.str() + "'";
    if (!isStopping) {
      hints.push_back({
          .kind = llvm::SourceMgr::DK_Remark,
          .loc = recursion.self.front()->loc(),
          .message =
              "how deep this tail recursion goes depends on data" + std::string(kWhileItRuns),
      });
      continue;
    }
    for (const auto &[call, known] : this->check.recursiveCalls) {
      if (known.target != fn || llvm::is_contained(recursion.self, call) ||
          (size_t(recursion.counter) < known.args.size() &&
           this->limitedTowards(known.args[recursion.counter], -recursion.step))) {
        continue;
      }
      hints.push_back({
          .kind = llvm::SourceMgr::DK_Remark,
          .loc = call->loc(),
          .message = "how deep the tail recursion of " + name + " goes depends on this argument" +
                     kWhileItRuns,
      });
    }
  }

  return hints;
}

std::vector<Diagnostic> LoopRanges::findLongLoops() {
  auto hints = this->findLongRecursions();
  for (const auto &[stmt, loop] : this->check.loops) {
    if (loop.bounded || this->waitsOrYields(stmt)) {
      continue;
    }
    const Expr *cond = nullptr;
    if (auto *whileLoop = dyn_cast<WhileStmt>(stmt)) {
      cond = whileLoop->cond;
    } else if (auto *doLoop = dyn_cast<DoWhileStmt>(stmt)) {
      cond = doLoop->cond;
    } else if (auto *forLoop = dyn_cast<ForStmt>(stmt)) {
      cond = forLoop->cond;
    }
    if (this->readsInput(cond)) {
      continue;
    }
    std::string data =
        loop.why ? " (" + this->check.overflowWarnings().inputText(loop.why, stmt->loc()) + ")"
                 : "";
    hints.push_back({
        .kind = llvm::SourceMgr::DK_Remark,
        .loc = stmt->loc(),
        .message = "how many times this loop runs depends on data" + data + kWhileItRuns,
    });
  }

  return hints;
}

std::vector<uint64_t> LoopRanges::contextOf() const {
  const OverflowCheck::Frame &frame = this->check.frames.back();
  std::vector<uint64_t> context = {this->check.quiet,
                                   this->check.frames.size(),
                                   uint64_t(reinterpret_cast<uintptr_t>(frame.fn)),
                                   frame.callback ? 1u : 0u,
                                   frame.quiet,
                                   frame.targets,
                                   this->check.targets.size()};
  for (size_t i = frame.targets; i < this->check.targets.size(); ++i) {
    context.push_back(this->check.targets[i].quiet);
  }

  return context;
}

bool LoopRanges::freshBases(const State &state, uint64_t before) const {
  for (const auto &entry : state.vars) {
    if (this->freshBase(entry.second, before)) {
      return true;
    }
  }

  return false;
}

void LoopRanges::runLoop(const OverflowCheck::LoopSpec &spec, State &st) {
  if (st.dead) {
    return;
  }
  if (this->check.quiet == 0 || this->check.overBudget()) {
    solveLoop(spec, st);
    return;
  }
  std::vector<uint64_t> context = contextOf();
  const std::vector<RememberedLoop> &known = this->remembered[spec.loc.getPointer()];
  OverflowCheck::Frame &frame = this->check.frames.back();
  size_t floor = frame.targets;
  for (const RememberedLoop &loop : known) {
    if (loop.context != context || this->check.work + loop.work > OverflowCheck::kWorkBudget ||
        loop.source.has_value() != spec.source.has_value() ||
        (loop.source && !this->identicalVal(*loop.source, *spec.source)) ||
        !this->identicalState(loop.entry, st)) {
      continue;
    }
    for (size_t i = 0; i < loop.jumps.size(); ++i) {
      OverflowCheck::Target &target = this->check.targets[floor + i];
      target.breaks = ValueRange::join(target.breaks, loop.jumps[i].first);
      target.conts = ValueRange::join(target.conts, loop.jumps[i].second);
    }
    frame.returned = ValueRange::join(frame.returned, loop.returned);
    frame.result = ValueRange::join(frame.result, loop.result);
    this->check.work += loop.work;
    st = loop.exit;
    return;
  }
  RememberedLoop loop;
  loop.context = std::move(context);
  loop.entry = st;
  loop.source = spec.source;
  std::vector<std::pair<State, State>> saved;
  for (size_t i = floor; i < this->check.targets.size(); ++i) {
    OverflowCheck::Target &target = this->check.targets[i];
    saved.push_back({std::move(target.breaks), std::move(target.conts)});
    target.breaks = ValueRange::deadState();
    target.conts = ValueRange::deadState();
  }
  State savedReturned = std::move(frame.returned);
  Val savedResult = std::move(frame.result);
  frame.returned = ValueRange::deadState();
  frame.result = Val();
  uint64_t workBefore = this->check.work;
  uint64_t basesBefore = this->check.bases_;
  solveLoop(spec, st);
  OverflowCheck::Frame &after = this->check.frames.back();
  bool isFresh = freshBases(st, basesBefore) || freshBases(after.returned, basesBefore) ||
                 this->freshBase(after.result, basesBefore);
  for (size_t i = 0; i < saved.size(); ++i) {
    OverflowCheck::Target &target = this->check.targets[floor + i];
    isFresh =
        isFresh || freshBases(target.breaks, basesBefore) || freshBases(target.conts, basesBefore);
    loop.jumps.push_back({target.breaks, target.conts});
    target.breaks = ValueRange::join(saved[i].first, target.breaks);
    target.conts = ValueRange::join(saved[i].second, target.conts);
  }
  loop.returned = after.returned;
  loop.result = after.result;
  after.returned = ValueRange::join(savedReturned, after.returned);
  after.result = ValueRange::join(savedResult, after.result);
  loop.work = this->check.work - workBefore;
  if (isFresh || this->check.overBudget()) {
    return;
  }
  loop.exit = st;
  std::vector<RememberedLoop> &rememberedLoops = this->remembered[spec.loc.getPointer()];
  if (rememberedLoops.size() >= kRememberedPerLoop) {
    rememberedLoops.erase(rememberedLoops.begin());
  }
  rememberedLoops.push_back(std::move(loop));
}

void LoopRanges::solveLoop(const OverflowCheck::LoopSpec &spec, State &st) {
  const State entry = st;
  ++this->check.quiet;
  llvm::DenseMap<const VarSymbol *, uint64_t> bases;

  State head = entry;
  State lastHead, lastEnd = ValueRange::deadState();
  bool isComingBack = false;
  int rounds = this->check.overBudget() ? 2 : 8;
  for (int k = 0; k < rounds; ++k) {
    bool isLast = k + 1 == rounds;
    State from = rebased(head, bases);
    State exits = ValueRange::deadState();
    State end = onePass(spec, from, exits);
    if (end.dead) {
      break;
    }
    isComingBack = true;
    bool isMoved = false;
    State next = ValueRange::join(entry, end);
    for (auto &headVar : head.vars) {
      const VarSymbol *var = headVar.first;
      auto found = next.vars.find(var);
      if (found == next.vars.end()) {
        continue;
      }
      Val &h = headVar.second;
      const Val &n = found->second;
      if (ValueRange::isBottom(h)) {
        if (!ValueRange::isBottom(n)) {
          h = n;
          h.num.base = h.len.base = 0;
          isMoved = true;
        }
        continue;
      }
      auto widen = [&](Num &to, const Num &got, Limits l) {
        if (to.empty() || got.empty()) {
          return;
        }
        static constexpr double kSteps[] = {
            0.0, 255.0, 65535.0, 16777215.0, 2147483647.0, 4294967295.0, 9007199254740992.0};
        auto up = [&](double v, double limit) {
          if (!isLast) {
            for (double t : kSteps) {
              double at = t == 0 ? -1 : t;
              if (v <= at && at < limit) {
                return at;
              }
            }
          }

          return limit;
        };
        auto down = [&](double v, double limit) {
          if (!isLast) {
            for (double t : kSteps) {
              double at = t == 0 ? 0 : -t - 1;
              if (v >= at && at > limit) {
                return at;
              }
            }
          }

          return limit;
        };
        if (got.lo < to.lo && to.lo > l.lo) {
          to.setLow({std::max(down(got.lo, l.lo), l.lo), got.loOrigin, got.loWhy});
          isMoved = true;
        }
        if (got.hi > to.hi && to.hi < l.hi) {
          to.setHigh({std::min(up(got.hi, l.hi), l.hi), got.hiOrigin, got.hiWhy});
          isMoved = true;
        }
        to.base = 0;
      };
      widen(h.num, n.num, ValueRange::limitsOf(var->type));
      widen(h.len, n.len, {0, kLengthMax});
      const Type *element =
          var->type->nonOptional()->isArray() ? var->type->nonOptional()->inner() : nullptr;
      if (h.elem && n.elem && !ValueRange::sameVal(*h.elem, ValueRange::join(*h.elem, *n.elem))) {
        h.elem = ValueRange::share(
            OverflowHelpers::widenVal(*h.elem, ValueRange::join(*h.elem, *n.elem), element));
        isMoved = true;
      } else if (h.elem && !n.elem) {
        h.elem = nullptr;
        isMoved = true;
      }
    }
    if (!isMoved) {
      break;
    }
  }
  if (!isComingBack) {
    --this->check.quiet;
    State exits = ValueRange::deadState();
    onePass(spec, entry, exits);
    st = exits;
    return;
  }

  for (int k = 0; k < (this->check.overBudget() ? 1 : 2); ++k) {
    State from = rebased(head, bases);
    State exits = ValueRange::deadState();
    State end = onePass(spec, from, exits);
    if (end.dead) {
      break;
    }
    lastEnd = end;
    lastHead = from;
    State next = ValueRange::join(entry, end);
    for (auto &headVar : head.vars) {
      auto found = next.vars.find(headVar.first);
      if (found == next.vars.end()) {
        continue;
      }
      Val &h = headVar.second;
      const Val &n = found->second;
      if (!h.num.empty() && !n.num.empty()) {
        h.num = ValueRange::meet(h.num, n.num);
      }
      if (!h.len.empty() && !n.len.empty()) {
        h.len = ValueRange::meet(h.len, n.len);
      }
      if (h.elem && n.elem && !h.elem->num.empty() && !n.elem->num.empty()) {
        Val el = *h.elem;
        el.num = ValueRange::meet(el.num, n.elem->num);
        h.elem = ValueRange::share(el);
      }
      h.num.base = h.len.base = 0;
    }
  }

  auto limit = [&](const Bound &moving, double at, bool unbounded, bool isAdditive = false) {
    if (moving.origin == Origin::Trusted || moving.origin == Origin::Input) {
      return Bound{at, moving.origin, moving.why};
    }
    if (unbounded && isAdditive && !spec.openEnded) {
      return Bound{at, Origin::Trusted, nullptr};
    }

    return Bound{at, Origin::Loop,
                 this->check.overflowWarnings().reason(Origin::Loop, spec.loc, "", unbounded)};
  };
  auto stepIn = [&](const VarSymbol *var, bool length) -> std::optional<std::pair<double, double>> {
    auto end = lastEnd.vars.find(var), from = lastHead.vars.find(var);
    if (lastEnd.dead || end == lastEnd.vars.end() || from == lastHead.vars.end()) {
      return std::nullopt;
    }

    return length ? this->stepOf(end->second.len, from->second.len.base)
                  : this->stepOf(end->second.num, from->second.num.base);
  };

  Num passes = passCount(spec, entry, head, lastHead, lastEnd);
  if ((passes.empty() || passes.hi > kUnrollPasses) && spec.simple && !this->check.overBudget()) {
    State at = entry;
    for (double k = 0; k < kUnrollPasses; ++k) {
      State exits = ValueRange::deadState();
      at = onePass(spec, at, exits);
      if (at.dead) {
        passes = ValueRange::range(0, k + 1);
        break;
      }
    }
  }
  noteLoop(spec, passes);
  State unrolledExits = ValueRange::deadState();
  bool isUnrolled = false;
  if (!passes.empty() && passes.hi <= kUnrollPasses && spec.simple && !this->check.overBudget()) {
    State at = entry;
    State all = entry;
    for (double k = 0; k <= passes.hi && !at.dead; ++k) {
      State exits = ValueRange::deadState();
      at = onePass(spec, at, exits);
      unrolledExits = ValueRange::join(unrolledExits, exits);
      all = ValueRange::join(all, at);
    }
    isUnrolled = at.dead;
    for (auto &headVar : head.vars) {
      auto found = all.vars.find(headVar.first);
      if (found == all.vars.end()) {
        continue;
      }
      Val v = found->second;
      v.num.base = v.len.base = 0;
      Limits l = ValueRange::limitsOf(headVar.first->type);
      const Num &start = entry.vars.lookup(headVar.first).num;
      if (!v.num.empty() && v.num.hi > l.hi) {
        v.num.setHigh(start.empty() || start.hi < l.hi ? limit(v.num.high(), l.hi, false)
                                                       : start.high());
      }
      if (!v.num.empty() && v.num.lo < l.lo) {
        v.num.setLow(start.empty() || start.lo > l.lo ? limit(v.num.low(), l.lo, false)
                                                      : start.low());
      }
      headVar.second = v;
    }
  } else {
    for (auto &headVar : head.vars) {
      const VarSymbol *var = headVar.first;
      auto end = lastEnd.vars.find(var);
      if (lastEnd.dead || end == lastEnd.vars.end()) {
        continue;
      }
      Val &h = headVar.second;
      const Val &start = entry.vars.lookup(var);
      bool isFloating = ValueRange::isFloatType(var->type);
      auto grow = [&](Num &n, const Num &startNum, const Num &endNum,
                      std::optional<std::pair<double, double>> step, Limits l) {
        if (n.empty() || startNum.empty() || endNum.empty()) {
          return;
        }
        if (n.hi >= l.hi && startNum.hi < l.hi) {
          if (step && !passes.empty() && step->second > 0) {
            double hi = ValueRange::plus(
                startNum.hi, ValueRange::times(std::max(passes.hi - 1, 0.0), step->second), kInf);
            if (hi <= l.hi) {
              n.setHigh(ValueRange::joint(hi, ValueRange::joint(hi, startNum.high(), endNum.high()),
                                          passes.high()));
            } else {
              n.setHigh(limit(ValueRange::joint(hi, endNum.high(), passes.high()), l.hi, false));
            }
          } else if (step && step->second > 0 &&
                     ((step->second <= 1 && l.hi >= kIntMax) ||
                      (isFloating && step->second <= kIntMax))) {
            n.setHigh(Bound{l.hi, Origin::Trusted, nullptr});
          } else {
            n.setHigh(limit(endNum.high(), l.hi, passes.empty(), step && step->second > 0));
          }
        }
        if (n.lo <= l.lo && startNum.lo > l.lo) {
          if (step && !passes.empty() && step->first < 0) {
            double lo = ValueRange::plus(
                startNum.lo, ValueRange::times(std::max(passes.hi - 1, 0.0), step->first), -kInf);
            if (lo >= l.lo) {
              n.setLow(ValueRange::joint(lo, ValueRange::joint(lo, startNum.low(), endNum.low()),
                                         passes.high()));
            } else {
              n.setLow(limit(ValueRange::joint(lo, endNum.low(), passes.high()), l.lo, false));
            }
          } else if (step && step->first < 0 &&
                     ((step->first >= -1 && l.lo <= kIntMin) ||
                      (isFloating && step->first >= -kIntMax))) {
            n.setLow(Bound{l.lo, Origin::Trusted, nullptr});
          } else {
            n.setLow(limit(endNum.low(), l.lo, passes.empty(), step && step->first < 0));
          }
        }
      };
      grow(h.num, start.num, end->second.num, stepIn(var, false), ValueRange::limitsOf(var->type));
      grow(h.len, start.len, end->second.len, stepIn(var, true), {0, kLengthMax});
      if (h.elem && start.elem && end->second.elem) {
        const Type *element =
            var->type->nonOptional()->isArray() ? var->type->nonOptional()->inner() : nullptr;
        Limits l = ValueRange::limitsOf(element);
        Val el = *h.elem;
        const Num &was = start.elem->num, &moved = end->second.elem->num;
        if (!el.num.empty() && !was.empty() && !moved.empty()) {
          if (el.num.hi >= l.hi && was.hi < l.hi) {
            el.num.setHigh(limit(moved.high(), l.hi, passes.empty()));
          }
          if (el.num.lo <= l.lo && was.lo > l.lo) {
            el.num.setLow(limit(moved.low(), l.lo, passes.empty()));
          }
          h.elem = ValueRange::share(el);
        }
      }
    }
  }

  for (auto &headVar : head.vars) {
    const Val &e = entry.vars.lookup(headVar.first);
    auto relate = [](Num &n, const Num &start, std::optional<std::pair<double, double>> step) {
      if (!step || n.empty() || start.empty()) {
        return;
      }
      if (step->first == 0 && step->second == 0) {
        n = start;
        return;
      }
      if (!start.base) {
        return;
      }
      n.base = start.base;
      n.offLo = ValueRange::plus(start.offLo, n.lo - start.lo, -kInf);
      n.offHi = ValueRange::plus(start.offHi, n.hi - start.hi, kInf);
    };
    relate(headVar.second.num, e.num, stepIn(headVar.first, false));
    relate(headVar.second.len, e.len, stepIn(headVar.first, true));
  }
  --this->check.quiet;

  State exits = ValueRange::deadState();
  onePass(spec, head, exits);
  st = isUnrolled ? unrolledExits : exits;
}

}
