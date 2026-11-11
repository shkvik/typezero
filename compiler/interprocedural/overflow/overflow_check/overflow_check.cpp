#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"
#include "compiler/interprocedural/overflow/assign_evaluator/assign_evaluator.h"
#include "compiler/interprocedural/overflow/builtin_evaluator/builtin_evaluator.h"
#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
#include "compiler/program/program/program.h"
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

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::dyn_cast_or_null;

using llvm::isa;

OverflowCheck::OverflowCheck(Program &checked, const LibraryRanges &library)
    : program_(checked.file),
      library(library),
      diags(checked.sourceMgr) {
  this->programScanPart = std::make_unique<ProgramScan>(*this);
  this->slotsPart = std::make_unique<Slots>(*this);
  this->callRangesPart = std::make_unique<CallRanges>(*this);
  this->executorPart = std::make_unique<Executor>(*this);
  this->loopRangesPart = std::make_unique<LoopRanges>(*this);
  this->narrowingPart = std::make_unique<Narrowing>(*this);
  this->evaluatorPart = std::make_unique<Evaluator>(*this);
  this->assignEvaluatorPart = std::make_unique<AssignEvaluator>(*this);
  this->builtinEvaluatorPart = std::make_unique<BuiltinEvaluator>(*this);
  this->overflowWarningsPart = std::make_unique<OverflowWarnings>(*this);
}

Diagnostics::ResultTaken OverflowCheck::run() {
  programScan().scanProgram();
  callRanges().analyzeAll();
  for (auto &entry : this->next.params) {
    std::vector<Slot> &slots = this->cur.params[entry.first];
    slots.resize(entry.second.size());
  }
  for (auto &entry : this->next.rets) {
    this->cur.rets[entry.first];
  }
  for (auto &entry : this->next.vars) {
    this->cur.vars[entry.first];
  }
  for (auto &entry : this->next.fields) {
    this->cur.fields[entry.first];
  }
  for (auto &entry : this->next.elements) {
    this->cur.elements[entry.first];
  }
  for (auto &entry : this->next.dicts) {
    this->cur.dicts[entry.first];
  }
  for (this->round = 1;; ++this->round) {
    this->next = Summaries();
    callRanges().analyzeAll();
    if (!callRanges().mergeSummaries(false)) {
      break;
    }
    if (this->round > kMaxRounds + 4) {
      break;
    }
  }
  for (int i = 0; i < 2; ++i) {
    ++this->round;
    this->next = Summaries();
    callRanges().analyzeAll();
    if (!callRanges().mergeSummaries(true)) {
      break;
    }
  }
  ++this->round;
  this->reporting = true;
  loopRanges().findRecursions();
  this->next = Summaries();
  callRanges().analyzeAll();

  llvm::SourceMgr &sources = this->diags.sourceMgr();
  std::stable_sort(this->warnings.begin(), this->warnings.end(),
                   [&](const Warning &a, const Warning &b) {
                     unsigned fa = sources.FindBufferContainingLoc(a.loc),
                              fb = sources.FindBufferContainingLoc(b.loc);
                     if (fa != fb) {
                       return fa < fb;
                     }

                     return a.loc.getPointer() < b.loc.getPointer();
                   });
  for (auto [expr, proven] : this->proofs) {
    expr->rangeProven = proven;
  }
  loopRanges().reportLong();
  std::set<const char *> shown;
  for (const Warning &w : this->warnings) {
    if (shown.insert(w.loc.getPointer()).second && !this->library.contains(w.loc)) {
      this->diags.error(w.loc, w.message);
    }
  }

  return this->diags.take();
}

void OverflowCheck::joinInto(Summaries &into, const Summaries &from) {
  for (const auto &[fn, slots] : from.params) {
    std::vector<Slot> &target = into.params[fn];
    if (target.size() < slots.size()) {
      target.resize(slots.size());
    }
    for (size_t i = 0; i < slots.size(); ++i) {
      target[i].val = ValueRange::join(target[i].val, slots[i].val);
    }
  }
  auto joinSlot = [](Slot &target, const Slot &slot) {
    target.val = ValueRange::join(target.val, slot.val);
    if (!target.type) {
      target.type = slot.type;
    }
  };
  for (const auto &[fn, slot] : from.rets) {
    joinSlot(into.rets[fn], slot);
  }
  for (const auto &[var, slot] : from.vars) {
    joinSlot(into.vars[var], slot);
  }
  for (const auto &[key, slot] : from.fields) {
    joinSlot(into.fields[key], slot);
  }
  for (const auto &[type, slot] : from.elements) {
    joinSlot(into.elements[type], slot);
  }
  for (const auto &[type, slot] : from.dicts) {
    joinSlot(into.dicts[type], slot);
  }
  auto joinFacts = [](std::vector<Num> &facts, const std::vector<Num> &more) {
    if (facts.size() < more.size()) {
      facts.resize(more.size());
    }
    for (size_t i = 0; i < more.size(); ++i) {
      facts[i] = ValueRange::join(facts[i], more[i]);
    }
  };
  for (const auto &[fn, pred] : from.preds) {
    Pred &target = into.preds[fn];
    joinFacts(target.whenTrue, pred.whenTrue);
    joinFacts(target.whenFalse, pred.whenFalse);
  }
  for (const auto &[key, calls] : from.dangerousCalls) {
    llvm::append_range(into.dangerousCalls[key], calls);
  }
}

OverflowCheck::~OverflowCheck() = default;

}
