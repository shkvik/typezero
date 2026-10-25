// Warnings where int arithmetic may overflow (docs/small.md, section 6).
//
// The pass runs after Checker, over the checked program, and runs every function
// abstractly: a number is a range [lo, hi] together with where its bounds come
// from, a string or an array has a range for its length, and an array a value
// for its elements. Conditions narrow the ranges (`if (x < 100)`), loops are
// run until their ranges settle, and values go from function to function
// through summaries of the whole program: the parameters of a function take
// what all its calls pass, a call gives what the function returns, and fields,
// elements of arrays, values of dictionaries and variables that several
// functions use take everything stored into them anywhere. The summaries are
// computed in rounds until they settle; the last round reports.
//
// Where the bounds of a value come from decides whether it is worth a warning
// (see Origin). A value from outside the program (parseInt, JSON) or one that
// grows with every pass of a loop without a bound may well be large; so may a
// value the pass computes exactly, such as `i * i` with i up to an array's
// length. But the program's own data that the pass does not follow closely (a
// field a loop increments, a result of recursion) is trusted: a warning about
// every `node.count + 1` would be noise.

#include "compiler/overflow/overflow_check/overflow_check.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/facts/facts.h"
#include "compiler/overflow/assign_evaluator/assign_evaluator.h"
#include "compiler/overflow/builtin_evaluator/builtin_evaluator.h"
#include "compiler/overflow/call_ranges/call_ranges.h"
#include "compiler/overflow/evaluator/evaluator.h"
#include "compiler/overflow/executor/executor.h"
#include "compiler/overflow/loop_ranges/loop_ranges.h"
#include "compiler/overflow/narrowing/narrowing.h"
#include "compiler/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/overflow/program_scan/program_scan.h"
#include "compiler/overflow/slots/slots.h"
#include "compiler/overflow/value_range/value_range.h"
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
  // The first round finds which summaries get values; their values start
  // empty after it.
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
    if (!callRanges().mergeSummaries(/*narrowing=*/false)) {
      break;
    }
    if (round_ > kMaxRounds + 4) {
      break;
    }
  }
  // Rounds from the settled summaries bring back the bounds that widening
  // gave up.
  for (int i = 0; i < 2; ++i) {
    ++round_;
    next_ = Summaries();
    callRanges().analyzeAll();
    if (!callRanges().mergeSummaries(/*narrowing=*/true)) {
      break;
    }
  }
  ++round_;
  reporting_ = true;
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
  // A place is reported once, also when several copies of a generic
  // function have it.
  std::set<const char *> shown;
  for (const Warning &w : warnings_) {
    if (shown.insert(w.loc.getPointer()).second) {
      diags_.warning(w.loc, w.message);
    }
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
} // namespace tz
