#include "compiler/library/regex/matcher_analysis/matcher_analysis.h"
#include "runtime/library/regex/regex/regex.h"
#include <array>
#include <map>
#include <set>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

namespace {

bool isBack(uint32_t op) {
  return op == RE_CHAR_BACK || op == RE_ANY_BACK || op == RE_ALL_BACK || op == RE_SET_BACK;
}

bool isCharInstruction(uint32_t op) {
  return op == RE_CHAR || op == RE_ANY || op == RE_ALL || op == RE_SET || isBack(op);
}

bool hasByte(const ByteSet &s, uint32_t c) { return (s[c / 32] >> (c % 32)) & 1; }

void addByte(ByteSet &s, uint32_t c) { s[c / 32] |= 1u << (c % 32); }

uint64_t counterRange(const CountedLoop &loop) {
  return loop.max == RE_INFINITY ? uint64_t(loop.min) + 1 : uint64_t(loop.max) + 1;
}

} // namespace

MatcherAnalysis::MatcherAnalysis(MatcherBuilder &builder) : builder_(builder) {}

void MatcherAnalysis::analyze() {
  builder_.leader_.assign(builder_.codeSize_ + 1, 0);
  builder_.scope_.assign(builder_.codeSize_ + 1, -1);
  builder_.leader_[0] = 1;
  std::vector<uint32_t> looks;
  for (uint32_t pc = 0; pc < builder_.codeSize_; pc += instructionSize(builder_.code_, pc)) {
    const uint32_t *op = builder_.code_ + pc;
    switch (op[0]) {
    case RE_GOTO:
    case RE_SPLIT_NEXT:
      builder_.leader_[op[1]] = 1;
      builder_.targets_.insert(op[1]);
      break;
    case RE_SPLIT_GOTO:
      builder_.leader_[op[1]] = builder_.leader_[pc + 2] = 1;
      builder_.targets_.insert(op[1]);
      builder_.targets_.insert(pc + 2);
      builder_.hasLoops_ = true;
      break;
    case RE_LOOP: {
      builder_.leader_[op[5]] = builder_.leader_[op[6]] = 1;
      builder_.targets_.insert(op[5]);
      builder_.targets_.insert(op[6]);
      const uint32_t *end = builder_.code_ + op[6] - 5; // its RE_LOOP_END
      builder_.loops_.push_back(
          {pc, op[1], op[2], op[3], op[5], op[6], end[3], end[2] != RE_INFINITY});
      builder_.hasLoops_ = true;
      break;
    }
    case RE_LOOP_END:
      builder_.leader_[op[4]] = 1;
      builder_.targets_.insert(op[4]);
      break;
    case RE_REPEAT:
      builder_.leader_[pc + instructionSize(builder_.code_, pc)] = 1;
      if (op[2] > 1) {
        builder_.hasLoops_ = true;
      }
      break;
    case RE_LOOK:
      builder_.leader_[op[2]] = 1;
      builder_.targets_.insert(op[2]);
      builder_.lookOfMatch_[op[2] - 1] = pc;
      looks.push_back(pc);
      builder_.hasLook_ = true;
      break;
    case RE_BACKREF:
      builder_.hasBackref_ = true;
      break;
    default:
      break;
    }
    if (op[0] == RE_SPLIT_NEXT) {
      builder_.hasLoops_ = true;
    }
  }
  // Inner lookarounds come later and overwrite the outer ones.
  for (uint32_t look : looks) {
    for (uint32_t pc = look + 3; pc < builder_.code_[look + 2]; ++pc) {
      builder_.scope_[pc] = int32_t(look);
    }
  }

  const uint32_t *first = builder_.program_ + builder_.program_[TZ_RE_H_FIRST];
  builder_.anchored_ = (first[8] & TZ_RE_FIRST_ANCHORED) != 0;

  // The skip past a run: a greedy unbounded repeat of a class first, with
  // nothing before it but the start of a group.
  uint32_t pc = 0;
  while (pc < builder_.codeSize_ && builder_.code_[pc] == RE_SET_POSITION) {
    pc += 2;
  }
  if (pc < builder_.codeSize_ && builder_.code_[pc] == RE_REPEAT && builder_.code_[pc + 3] &&
      builder_.code_[pc + 2] == RE_INFINITY && !builder_.hasBackref_ &&
      !isBack(builder_.code_[pc + 5]) && (pc == 0 || !builder_.targets_.count(pc))) {
    builder_.skipRepeat_ = pc;
  }
}

bool MatcherAnalysis::inLoop(const CountedLoop &loop, uint32_t pc) const {
  return pc >= loop.pc && pc < loop.exit && builder_.scope_[loop.pc] == builder_.scope_[pc];
}

// Whether at `pc` it matters for `loop` if the iteration has matched
// anything yet: past the start of the body of a loop with the empty check.
bool MatcherAnalysis::flagged(const CountedLoop &loop, uint32_t pc) const {
  return loop.emptyCheck && pc > loop.body;
}

// How many states the counted loops around `pc` can be in.
uint64_t MatcherAnalysis::contextSize(uint32_t pc) const {
  uint64_t size = 1;
  for (const CountedLoop &loop : builder_.loops_) {
    if (!inLoop(loop, pc)) {
      continue;
    }
    size *= counterRange(loop) * (flagged(loop, pc) ? 2 : 1);
    if (size > kMaxMemoRows) {
      return size;
    }
  }

  return size;
}

void MatcherAnalysis::planMemo() {
  builder_.memo_ = !builder_.hasBackref_ && builder_.hasLoops_;
  if (!builder_.memo_) {
    return;
  }
  uint64_t rows = 0;
  for (uint32_t pc = 0; pc < builder_.codeSize_; pc += instructionSize(builder_.code_, pc)) {
    if (builder_.leader_[pc]) {
      builder_.blockRow_[pc] = uint32_t(rows);
      rows += contextSize(pc);
    }
    if (builder_.code_[pc] == RE_REPEAT && builder_.code_[pc + 2] == RE_INFINITY) {
      builder_.repeatRow_[pc] = uint32_t(rows);
      rows += contextSize(pc);
    }
    if (rows > kMaxMemoRows) {
      builder_.memo_ = false;
      return;
    }
  }
  builder_.rows_ = uint32_t(rows);
}

// Adds to `states` what the program gets to from them without reading a
// character: the states that read one, and whether the match may end
// (`match`) or what comes is not known (`unknown`, a backreference).
void MatcherAnalysis::closure(std::set<uint32_t> &states, bool &match, bool &unknown) const {
  std::vector<uint32_t> work(states.begin(), states.end());
  std::set<uint32_t> seen(states.begin(), states.end());
  states.clear();
  auto add = [&](uint32_t pc) {
    if (seen.insert(pc).second) {
      work.push_back(pc);
    }
  };
  while (!work.empty()) {
    uint32_t pc = work.back();
    work.pop_back();
    const uint32_t *op = builder_.code_ + pc;
    uint32_t next = pc + instructionSize(builder_.code_, pc);
    switch (op[0]) {
    case RE_MATCH:
      match = true;
      break;
    case RE_FAIL:
      break;
    case RE_GOTO:
      add(op[1]);
      break;
    case RE_SPLIT_NEXT:
    case RE_SPLIT_GOTO:
      add(op[1]);
      add(pc + 2);
      break;
    case RE_LOOP:
      add(op[5]);
      add(op[6]);
      break;
    case RE_LOOP_END:
      add(op[4]);
      break;
    case RE_LOOK:
      add(op[2]); // what the lookaround allows is a part of all this
      break;
    case RE_REPEAT:
      states.insert(pc);
      if (op[1] == 0) {
        add(next);
      }
      break;
    case RE_BACKREF:
      unknown = true;
      break;
    default:
      if (isCharInstruction(op[0])) {
        states.insert(pc);
      } else {
        add(next); // registers, captures, assertions
      }
      break;
    }
  }
}

// The units a match can have at offsets 0, 1, ... of it, as long as every
// match is longer: for the scan for where one starts.
std::vector<OffsetSet> MatcherAnalysis::offsetSets(unsigned limit) const {
  std::vector<OffsetSet> sets;
  std::set<uint32_t> states = {0};
  while (sets.size() < limit) {
    bool match = false, unknown = false;
    closure(states, match, unknown);
    if (match || unknown || states.empty()) {
      break;
    }
    OffsetSet units;
    ByteSet &bytes = units.bytes;
    std::set<uint32_t> next;
    for (uint32_t pc : states) {
      const uint32_t *op = builder_.code_ + pc;
      uint32_t atom = pc, after = pc + instructionSize(builder_.code_, pc);
      if (op[0] == RE_REPEAT) {
        atom = pc + 5;
        if (op[2] > 1) {
          next.insert(pc);
        }
      }
      const uint32_t *a = builder_.code_ + atom;
      switch (a[0]) {
      case RE_CHAR:
        if (a[1] < 256) {
          addByte(bytes, a[1]);
        } else {
          units.wide = true;
        }
        break;
      case RE_ANY:
        for (uint32_t c = 0; c < 256; ++c) {
          if (c != '\n' && c != '\r') {
            addByte(bytes, c);
          }
        }
        units.wide = true;
        break;
      case RE_SET:
        for (int i = 0; i < 8; ++i) {
          bytes[i] |= a[2 + i];
        }
        if (a[1] > 0) {
          units.wide = true;
        }
        break;
      default: // RE_ALL; the back forms come only in lookbehinds
        bytes.fill(0xFFFFFFFFu);
        units.wide = true;
        break;
      }
      next.insert(after);
    }
    sets.push_back(units);
    states = next;
  }

  return sets;
}

void MatcherAnalysis::planScan() {
  if (builder_.anchored_) {
    return;
  }
  std::vector<OffsetSet> sets = offsetSets(16);
  struct Choice {
    uint32_t offset;
    double share; // of the bytes of text in the set
  };
  std::vector<Choice> usable;
  for (uint32_t k = 0; k < sets.size(); ++k) {
    double share = 0;
    bool any = false;
    for (uint32_t c = 0; c < 256; ++c) {
      if (hasByte(sets[k].bytes, c)) {
        share += byteFrequency(c);
        any = true;
      }
    }
    if (!any) {
      builder_.scanEmpty_ = true; // in a byte string nothing can come there
    }
    if (any && !byteTests(sets[k].bytes).empty()) {
      usable.push_back({k, share / 1000});
    }
  }
  std::stable_sort(usable.begin(), usable.end(),
                   [](const Choice &a, const Choice &b) { return a.share < b.share; });
  // The rarest offsets, up to three, until about one start in 256 is left.
  double share = 1;
  for (const Choice &choice : usable) {
    if (builder_.scan_.size() == 3 || (builder_.scan_.size() >= 2 && share < 1.0 / 256)) {
      break;
    }
    share *= choice.share;
    builder_.scan_.push_back({choice.offset, sets[choice.offset]});
  }
  // Where nearly every byte may start a match, the scan by units is faster.
  if (share > 0.1) {
    builder_.scan_.clear();
  }
  uint32_t reach = 0;
  for (const ScanOffset &s : builder_.scan_) {
    reach = std::max(reach, s.offset);
  }
  builder_.scanUnits_ = !builder_.scan_.empty();
  for (uint32_t k = 0; builder_.unicode_ && k <= reach && k < sets.size(); ++k) {
    builder_.scanUnits_ &= !sets[k].wide;
  }
}

} // namespace tz
