#include "compiler/pipeline/check_program/overflow/overflow_check/overflow_check.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/facts/facts.h"
#include "compiler/pipeline/check_program/overflow/assign_evaluator/assign_evaluator.h"
#include "compiler/pipeline/check_program/overflow/builtin_evaluator/builtin_evaluator.h"
#include "compiler/pipeline/check_program/overflow/call_ranges/call_ranges.h"
#include "compiler/pipeline/check_program/overflow/evaluator/evaluator.h"
#include "compiler/pipeline/check_program/overflow/executor/executor.h"
#include "compiler/pipeline/check_program/overflow/loop_ranges/loop_ranges.h"
#include "compiler/pipeline/check_program/overflow/narrowing/narrowing.h"
#include "compiler/pipeline/check_program/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/pipeline/check_program/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/pipeline/check_program/overflow/program_scan/program_scan.h"
#include "compiler/pipeline/check_program/overflow/slots/slots.h"
#include "compiler/pipeline/check_program/overflow/value_range/value_range.h"
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

void OverflowCheck::run() {
  programScan().scanProgram();
  callRanges().analyzeAll();
  for (auto &entry : next_.params) {
    std::vector<Slot> &slots = cur_.params[entry.first];
    slots.resize(entry.second.size());
  }
  for (auto &entry : next_.rets) {
    cur_.rets[entry.first];
  }
  for (auto &entry : next_.vars) {
    cur_.vars[entry.first];
  }
  for (auto &entry : next_.fields) {
    cur_.fields[entry.first];
  }
  for (auto &entry : next_.elements) {
    cur_.elements[entry.first];
  }
  for (auto &entry : next_.dicts) {
    cur_.dicts[entry.first];
  }
  for (round_ = 1;; ++round_) {
    next_ = Summaries();
    callRanges().analyzeAll();
    if (!callRanges().mergeSummaries(false)) {
      break;
    }
    if (round_ > kMaxRounds + 4) {
      break;
    }
  }
  for (int i = 0; i < 2; ++i) {
    ++round_;
    next_ = Summaries();
    callRanges().analyzeAll();
    if (!callRanges().mergeSummaries(true)) {
      break;
    }
  }
  ++round_;
  reporting_ = true;
  loopRanges().findRecursions();
  next_ = Summaries();
  callRanges().analyzeAll();

  llvm::SourceMgr &sources = diags_.sourceMgr();
  std::stable_sort(warnings_.begin(), warnings_.end(), [&](const Warning &a, const Warning &b) {
    unsigned fa = sources.FindBufferContainingLoc(a.loc),
             fb = sources.FindBufferContainingLoc(b.loc);
    if (fa != fb) {
      return fa < fb;
    }

    return a.loc.getPointer() < b.loc.getPointer();
  });
  for (auto [expr, proven] : proofs_) {
    expr->rangeProven = proven;
  }
  loopRanges().reportLong();
  std::set<const char *> shown;
  for (const Warning &w : warnings_) {
    if (shown.insert(w.loc.getPointer()).second && !diags_.isQuiet(w.loc)) {
      diags_.error(w.loc, w.message);
    }
  }
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

void checkIntOverflow(SourceFile &program, Diagnostics &diags) {
  OverflowCheck(program, diags).run();
}

OverflowCheck::OverflowCheck(SourceFile &program, Diagnostics &diags)
    : program_(program), diags_(diags) {
  programScanPart_ = std::make_unique<ProgramScan>(*this);
  slotsPart_ = std::make_unique<Slots>(*this);
  callRangesPart_ = std::make_unique<CallRanges>(*this);
  executorPart_ = std::make_unique<Executor>(*this);
  loopRangesPart_ = std::make_unique<LoopRanges>(*this);
  narrowingPart_ = std::make_unique<Narrowing>(*this);
  evaluatorPart_ = std::make_unique<Evaluator>(*this);
  assignEvaluatorPart_ = std::make_unique<AssignEvaluator>(*this);
  builtinEvaluatorPart_ = std::make_unique<BuiltinEvaluator>(*this);
  overflowWarningsPart_ = std::make_unique<OverflowWarnings>(*this);
}

OverflowCheck::~OverflowCheck() = default;

}
