#include "compiler/lower/library/regex/matcher_analysis/matcher_analysis.h"
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

MatcherAnalysis::MatcherAnalysis(MatcherBuilder &builder)
    : builder(builder) {}

void MatcherAnalysis::analyze() {
  this->builder.leader.assign(this->builder.codeSize + 1, 0);
  this->builder.scope.assign(this->builder.codeSize + 1, -1);
  this->builder.leader[0] = 1;
  std::vector<uint32_t> looks;
  for (uint32_t pc = 0; pc < this->builder.codeSize;
       pc += MatcherBuilder::instructionSize(this->builder.code, pc)) {
    const uint32_t *op = this->builder.code + pc;
    switch (op[0]) {
    case RE_GOTO:
    case RE_SPLIT_NEXT:
      this->builder.leader[op[1]] = 1;
      this->builder.targets.insert(op[1]);
      break;
    case RE_SPLIT_GOTO:
      this->builder.leader[op[1]] = this->builder.leader[pc + 2] = 1;
      this->builder.targets.insert(op[1]);
      this->builder.targets.insert(pc + 2);
      this->builder.hasLoops = true;
      break;
    case RE_LOOP: {
      this->builder.leader[op[5]] = this->builder.leader[op[6]] = 1;
      this->builder.targets.insert(op[5]);
      this->builder.targets.insert(op[6]);
      const uint32_t *end = this->builder.code + op[6] - 5;
      this->builder.loops.push_back(
          {pc, op[1], op[2], op[3], op[5], op[6], end[3], end[2] != RE_INFINITY});
      this->builder.hasLoops = true;
      break;
    }
    case RE_LOOP_END:
      this->builder.leader[op[4]] = 1;
      this->builder.targets.insert(op[4]);
      break;
    case RE_REPEAT:
      this->builder.leader[pc + MatcherBuilder::instructionSize(this->builder.code, pc)] = 1;
      if (op[2] > 1) {
        this->builder.hasLoops = true;
      }
      break;
    case RE_LOOK:
      this->builder.leader[op[2]] = 1;
      this->builder.targets.insert(op[2]);
      this->builder.lookOfMatch[op[2] - 1] = pc;
      looks.push_back(pc);
      this->builder.hasLook = true;
      break;
    case RE_BACKREF:
      this->builder.hasBackref = true;
      break;
    default:
      break;
    }
    if (op[0] == RE_SPLIT_NEXT) {
      this->builder.hasLoops = true;
    }
  }
  for (uint32_t look : looks) {
    for (uint32_t pc = look + 3; pc < this->builder.code[look + 2]; ++pc) {
      this->builder.scope[pc] = int32_t(look);
    }
  }

  const uint32_t *first = this->builder.program_ + this->builder.program_[TZ_RE_H_FIRST];
  this->builder.anchored = (first[8] & TZ_RE_FIRST_ANCHORED) != 0;

  uint32_t pc = 0;
  while (pc < this->builder.codeSize && this->builder.code[pc] == RE_SET_POSITION) {
    pc += 2;
  }
  if (pc < this->builder.codeSize && this->builder.code[pc] == RE_REPEAT &&
      this->builder.code[pc + 3] && this->builder.code[pc + 2] == RE_INFINITY &&
      !this->builder.hasBackref && !MatcherBuilder::isBack(this->builder.code[pc + 5]) &&
      (pc == 0 || !this->builder.targets.count(pc))) {
    this->builder.skipRepeat = pc;
  }
}

bool MatcherAnalysis::inLoop(const CountedLoop &loop, uint32_t pc) const {
  return pc >= loop.pc && pc < loop.exit && this->builder.scope[loop.pc] == this->builder.scope[pc];
}

bool MatcherAnalysis::flagged(const CountedLoop &loop, uint32_t pc) const {
  return loop.emptyCheck && pc > loop.body;
}

uint64_t MatcherAnalysis::contextSize(uint32_t pc) const {
  uint64_t size = 1;
  for (const CountedLoop &loop : this->builder.loops) {
    if (!inLoop(loop, pc)) {
      continue;
    }
    size *= MatcherBuilder::counterRange(loop) * (flagged(loop, pc) ? 2 : 1);
    if (size > kMaxMemoRows) {
      return size;
    }
  }

  return size;
}

void MatcherAnalysis::planMemo() {
  this->builder.memo = !this->builder.hasBackref && this->builder.hasLoops;
  if (!this->builder.memo) {
    return;
  }
  uint64_t rows = 0;
  for (uint32_t pc = 0; pc < this->builder.codeSize;
       pc += MatcherBuilder::instructionSize(this->builder.code, pc)) {
    if (this->builder.leader[pc]) {
      this->builder.blockRow[pc] = uint32_t(rows);
      rows += contextSize(pc);
    }
    if (this->builder.code[pc] == RE_REPEAT && this->builder.code[pc + 2] == RE_INFINITY) {
      this->builder.repeatRow[pc] = uint32_t(rows);
      rows += contextSize(pc);
    }
    if (rows > kMaxMemoRows) {
      this->builder.memo = false;
      return;
    }
  }
  this->builder.rows = uint32_t(rows);
}

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
    const uint32_t *op = this->builder.code + pc;
    uint32_t next = pc + MatcherBuilder::instructionSize(this->builder.code, pc);
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
      add(op[2]);
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
      if (MatcherBuilder::isCharInstruction(op[0])) {
        states.insert(pc);
      } else {
        add(next);
      }
      break;
    }
  }
}

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
      const uint32_t *op = this->builder.code + pc;
      uint32_t atom = pc, after = pc + MatcherBuilder::instructionSize(this->builder.code, pc);
      if (op[0] == RE_REPEAT) {
        atom = pc + 5;
        if (op[2] > 1) {
          next.insert(pc);
        }
      }
      const uint32_t *a = this->builder.code + atom;
      switch (a[0]) {
      case RE_CHAR:
        if (a[1] < 256) {
          MatcherBuilder::addByte(bytes, a[1]);
        } else {
          units.wide = true;
        }
        break;
      case RE_ANY:
        for (uint32_t c = 0; c < 256; ++c) {
          if (c != '\n' && c != '\r') {
            MatcherBuilder::addByte(bytes, c);
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
      default:
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
  if (this->builder.anchored) {
    return;
  }
  std::vector<OffsetSet> sets = offsetSets(16);
  struct Choice {
    uint32_t offset;
    double share;
  };
  std::vector<Choice> usable;
  for (uint32_t k = 0; k < sets.size(); ++k) {
    double share = 0;
    bool any = false;
    for (uint32_t c = 0; c < 256; ++c) {
      if (MatcherBuilder::hasByte(sets[k].bytes, c)) {
        share += MatcherBuilder::byteFrequency(c);
        any = true;
      }
    }
    if (!any) {
      this->builder.scanEmpty = true;
    }
    if (any && !MatcherBuilder::byteTests(sets[k].bytes).empty()) {
      usable.push_back({k, share / 1000});
    }
  }
  std::stable_sort(usable.begin(), usable.end(),
                   [](const Choice &a, const Choice &b) { return a.share < b.share; });
  double share = 1;
  for (const Choice &choice : usable) {
    if (this->builder.scan.size() == 3 || (this->builder.scan.size() >= 2 && share < 1.0 / 256)) {
      break;
    }
    share *= choice.share;
    this->builder.scan.push_back({choice.offset, sets[choice.offset]});
  }
  if (share > 0.1) {
    this->builder.scan.clear();
  }
  uint32_t reach = 0;
  for (const ScanOffset &s : this->builder.scan) {
    reach = std::max(reach, s.offset);
  }
  this->builder.scanUnits = !this->builder.scan.empty();
  for (uint32_t k = 0; this->builder.unicode && k <= reach && k < sets.size(); ++k) {
    this->builder.scanUnits &= !sets[k].wide;
  }
}

}
