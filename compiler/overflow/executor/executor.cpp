#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/check/facts/facts.h"
#include "compiler/check/sema/sema.h"
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
#include "compiler/overflow/value_range/value_range.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

Executor::Executor(OverflowCheck &check) : check_(check) {}

void Executor::exec(const Stmt *stmt, State &st) {
  if (!stmt || st.dead) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      if (st.dead) {
        break;
      }
      exec(s, st);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      if (!d.sym) {
        continue;
      }
      Val v = d.init ? check_.evaluator().eval(d.init, st) : Val();
      check_.slots().writeVar(d.sym, v, st);
    }
    break;
  case Stmt::Kind::Function:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Debugger:
    break;
  case Stmt::Kind::Expr:
    check_.evaluator().eval(cast<ExprStmt>(stmt)->expr, st);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    State t, f;
    check_.narrowing().refine(s->cond, st, t, f);
    exec(s->thenStmt, t);
    exec(s->elseStmt, f);
    st = join(t, f);
    break;
  }
  case Stmt::Kind::While:
  case Stmt::Kind::DoWhile:
  case Stmt::Kind::For:
  case Stmt::Kind::ForOf:
    check_.loopRanges().execLoop(stmt, {}, st);
    break;
  case Stmt::Kind::Switch:
    execSwitch(cast<SwitchStmt>(stmt), st);
    break;
  case Stmt::Kind::Labeled:
    execLabeled(cast<LabeledStmt>(stmt), st);
    break;
  case Stmt::Kind::Return:
    execReturn(cast<ReturnStmt>(stmt), st);
    break;
  case Stmt::Kind::Break:
    jump(stmt, cast<BreakStmt>(stmt)->label, true, st);
    break;
  case Stmt::Kind::Continue:
    jump(stmt, cast<ContinueStmt>(stmt)->label, false, st);
    break;
  case Stmt::Kind::Throw:
    check_.evaluator().eval(cast<ThrowStmt>(stmt)->value, st);
    check_.mayThrow(st);
    st.dead = true;
    break;
  case Stmt::Kind::Try:
    execTry(cast<TryStmt>(stmt), st);
    break;
  case Stmt::Kind::Class: {
    // Declaring a class sets its static fields.
    auto *cls = cast<ClassDecl>(stmt);
    if (cls->builtin) {
      break;
    }
    for (const ClassField &f : cls->fields) {
      if (f.isStatic && f.var && f.init) {
        check_.slots().writeVar(f.var, check_.evaluator().eval(f.init, st), st);
      }
    }
    break;
  }
  }
}

void Executor::execLabeled(const LabeledStmt *stmt, State &st) {
  llvm::SmallVector<llvm::StringRef, 1> labels{stmt->label};
  const Stmt *body = stmt->body;
  while (auto *inner = dyn_cast<LabeledStmt>(body)) {
    labels.push_back(inner->label);
    body = inner->body;
  }
  if (isa<WhileStmt>(body) || isa<DoWhileStmt>(body) || isa<ForStmt>(body) ||
      isa<ForOfStmt>(body)) {
    check_.loopRanges().execLoop(body, labels, st);
    return;
  }
  OverflowCheck::Target target;
  target.block = true;
  target.labels = labels;
  target.quiet = check_.quiet_;
  check_.targets_.push_back(std::move(target));
  exec(body, st);
  State breaks = check_.targets_.back().breaks;
  check_.targets_.pop_back();
  st = join(st, breaks);
}

void Executor::jump(const Stmt *, llvm::StringRef label, bool isBreak, State &st) {
  size_t floor = check_.frames_.back().targets;
  for (size_t i = check_.targets_.size(); i > floor; --i) {
    OverflowCheck::Target &t = check_.targets_[i - 1];
    bool matches =
        label.empty() ? (isBreak ? !t.block : t.loop) : llvm::is_contained(t.labels, label);
    if (!matches) {
      continue;
    }
    if (t.quiet != check_.quiet_) {
      break;
    }
    if (isBreak) {
      t.breaks = join(t.breaks, st);
    } else {
      t.conts = join(t.conts, st);
    }
    break;
  }
  st.dead = true;
}

void Executor::execSwitch(const SwitchStmt *stmt, State &st) {
  check_.evaluator().eval(stmt->discriminant, st);
  for (const SwitchCase &c : stmt->cases) {
    if (c.test) {
      check_.evaluator().eval(c.test, st);
    }
  }
  OverflowCheck::Target target;
  target.quiet = check_.quiet_;
  check_.targets_.push_back(std::move(target));
  State fall = deadState();
  bool hasDefault = false;
  for (const SwitchCase &c : stmt->cases) {
    if (!c.test) {
      hasDefault = true;
    }
    State in = join(fall, st);
    for (const Stmt *s : c.body) {
      if (in.dead) {
        break;
      }
      exec(s, in);
    }
    fall = in;
  }
  State breaks = check_.targets_.back().breaks;
  check_.targets_.pop_back();
  State out = join(fall, breaks);
  if (!hasDefault && !stmt->exhaustive) {
    out = join(out, st);
  }
  st = out;
}

void Executor::execTry(const TryStmt *stmt, State &st) {
  State entry = st;
  State thrown = deadState();
  check_.tries_.push_back({&thrown, check_.quiet_});
  exec(stmt->block, st);
  check_.tries_.pop_back();
  State out = st;
  if (stmt->handler) {
    // Something in the block may throw at any point.
    State handler = join(join(entry, st), thrown);
    exec(stmt->handler, handler);
    out = join(st, handler);
  }
  if (stmt->finalizer) {
    if (out.dead) {
      State fin = join(entry, thrown);
      exec(stmt->finalizer, fin);
    } else {
      exec(stmt->finalizer, out);
    }
  }
  st = out;
}

void Executor::execReturn(const ReturnStmt *ret, State &st) {
  // The value may run callbacks, which push frames.
  size_t at = check_.frames_.size() - 1;
  Val v;
  if (ret->value) {
    const FunctionDecl *fn = check_.frames_[at].fn;
    const Type *retType = fn && fn->sym ? fn->sym->ret : nullptr;
    if (!check_.frames_[at].callback && retType && retType->isBool()) {
      // A function that tests: what holds when it returns true, and false.
      State t, f;
      check_.narrowing().refine(ret->value, st, t, f);
      check_.narrowing().notePredicate(fn, t, f);
    } else {
      v = check_.evaluator().eval(ret->value, st);
    }
  }
  OverflowCheck::Frame &frame = check_.frames_[at];
  if (frame.callback) {
    if (frame.quiet == check_.quiet_) {
      frame.result = join(frame.result, v);
      frame.returned = join(frame.returned, st);
    }
  } else if (frame.fn && ret->value && !check_.quiet_) {
    const Type *retType = frame.fn->sym ? frame.fn->sym->ret : nullptr;
    if (tracked(retType)) {
      check_.contribute(check_.next_.rets[frame.fn], check_.overflowWarnings().fitVal(v, retType));
    }
  }
  st.dead = true;
}

} // namespace tz
