#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
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

Executor::Executor(OverflowCheck &check)
    : check(check) {}

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
      Val v = d.init ? this->check.evaluator().eval(d.init, st) : Val();
      this->check.slots().writeVar(d.sym, v, st);
    }
    break;
  case Stmt::Kind::Function:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Debugger:
    break;
  case Stmt::Kind::Expr:
    this->check.evaluator().eval(cast<ExprStmt>(stmt)->expr, st);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    State t, f;
    this->check.narrowing().refine(s->cond, st, t, f);
    exec(s->thenStmt, t);
    exec(s->elseStmt, f);
    st = ValueRange::join(t, f);
    break;
  }
  case Stmt::Kind::While:
  case Stmt::Kind::DoWhile:
  case Stmt::Kind::For:
  case Stmt::Kind::ForOf:
    this->check.loopRanges().execLoop(stmt, {}, st);
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
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(stmt);
    if (cls->builtin()) {
      break;
    }
    for (const ClassField &f : cls->fields) {
      if (f.isStatic() && f.var() && f.init) {
        this->check.slots().writeVar(f.var(), this->check.evaluator().eval(f.init, st), st);
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
    this->check.loopRanges().execLoop(body, labels, st);
    return;
  }
  OverflowCheck::Target target;
  target.block = true;
  target.labels = labels;
  target.quiet = this->check.quiet;
  this->check.targets.push_back(std::move(target));
  exec(body, st);
  State breaks = this->check.targets.back().breaks;
  this->check.targets.pop_back();
  st = ValueRange::join(st, breaks);
}

void Executor::jump(const Stmt *, llvm::StringRef label, bool isBreak, State &st) {
  size_t floor = this->check.frames.back().targets;
  for (size_t i = this->check.targets.size(); i > floor; --i) {
    OverflowCheck::Target &t = this->check.targets[i - 1];
    bool isMatching =
        label.empty() ? (isBreak ? !t.block : t.loop) : llvm::is_contained(t.labels, label);
    if (!isMatching) {
      continue;
    }
    if (t.quiet != this->check.quiet) {
      break;
    }
    if (isBreak) {
      t.breaks = ValueRange::join(t.breaks, st);
    } else {
      t.conts = ValueRange::join(t.conts, st);
    }
    break;
  }
  st.dead = true;
}

void Executor::execSwitch(const SwitchStmt *stmt, State &st) {
  this->check.evaluator().eval(stmt->discriminant, st);
  for (const SwitchCase &c : stmt->cases) {
    if (c.test) {
      this->check.evaluator().eval(c.test, st);
    }
  }
  OverflowCheck::Target target;
  target.quiet = this->check.quiet;
  this->check.targets.push_back(std::move(target));
  State fall = ValueRange::deadState();
  bool hasDefault = false;
  for (const SwitchCase &c : stmt->cases) {
    if (!c.test) {
      hasDefault = true;
    }
    State in = ValueRange::join(fall, st);
    for (const Stmt *s : c.body) {
      if (in.dead) {
        break;
      }
      exec(s, in);
    }
    fall = in;
  }
  State breaks = this->check.targets.back().breaks;
  this->check.targets.pop_back();
  State out = ValueRange::join(fall, breaks);
  if (!hasDefault && !stmt->exhaustive) {
    out = ValueRange::join(out, st);
  }
  st = out;
}

void Executor::execReturn(const ReturnStmt *ret, State &st) {
  size_t at = this->check.frames.size() - 1;
  Val v;
  if (ret->value) {
    const FunctionDecl *fn = this->check.frames[at].fn;
    const Type *retType = fn && fn->sym ? fn->sym->getKnownReturnType() : nullptr;
    if (!this->check.frames[at].callback && retType && retType->isBool()) {
      State t, f;
      this->check.narrowing().refine(ret->value, st, t, f);
      this->check.narrowing().notePredicate(fn, t, f);
    } else {
      v = this->check.evaluator().eval(ret->value, st);
    }
  }
  OverflowCheck::Frame &frame = this->check.frames[at];
  if (frame.callback) {
    if (frame.quiet == this->check.quiet) {
      frame.result = ValueRange::join(frame.result, v);
      frame.returned = ValueRange::join(frame.returned, st);
    }
  } else if (frame.fn && ret->value && !this->check.quiet) {
    const Type *retType = frame.fn->sym ? frame.fn->sym->getKnownReturnType() : nullptr;
    if (ValueRange::tracked(retType)) {
      this->check.contribute(this->check.next.rets[frame.fn],
                             this->check.overflowWarnings().fitVal(v, retType));
    }
  }
  st.dead = true;
}

}
