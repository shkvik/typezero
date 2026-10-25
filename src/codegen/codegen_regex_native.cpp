// Regex literals as machine code (docs/regex.md, section 1).
//
// tz turns the program of each regex literal (runtime/regex.h) into a
// matcher of its own (TzReMatch, runtime/tzrt.h): LLVM IR that is optimized
// with the rest of the program, so the interpreter of runtime/regex.c is
// left to the regexes of `new RegExp(s)`. The matcher is the program
// translated instruction by instruction, each doing what the interpreter
// does for it, with the same choice points and undo records on a stack
// (tz_re_bt), so its results are the interpreter's, which are JavaScript's.
// What goes is the cost of interpreting: there is no dispatch on opcodes,
// the operands are constants, a class is a few compares or a table, and the
// code comes twice, for strings of bytes and of two-byte units.
//
// Shortcuts make it faster still, each skipping only work that would fail
// with nothing to show for it, so the results stay the same:
// - Where a match may start is found by a scan. In a byte string it reads
//   32 bytes at a time for the bytes a match can have at up to three
//   offsets from its start (`(\d{4})-` has `-` at offset 4); otherwise it
//   reads unit by unit for those a match can start with.
// - A choice between two ways looks at the unit at the position first: a way
//   that cannot start with it is not taken, nor kept for later, so the
//   alternatives of `error|warning|fatal` cost one compare each.
// - A run of characters is compared 8 bytes at a time, both cases of a
//   letter under i at once.
// - A greedy repeat followed by a character, as in `[a-z]+ing`, ends at the
//   last place that character is, and a regex that starts with a greedy
//   repeat of a class without an upper bound is tried only once in a run of
//   that class: when no match starts at the run's first character, none
//   starts at the others either (the repeat can end at fewer places, the
//   rest of the pattern is the same), so the next try is past the run.
// - With g, one call finds many matches (TzReMatch).
//
// And backtracking does not blow up. Whether the rest of the program
// matches from an instruction at a position depends on nothing else but the
// registers of counted loops and the captures that backreferences read; the
// other captures change only what the match reports. So once the work of a
// search (choices taken, loop iterations, characters repeats read) passes a
// budget, the matcher notes each instruction it gets to, with the state of
// the counted loops around it, and the position, in a bit set (TzReMemo in
// runtime/regex.h). Where it gets to a noted one again, it fails at once:
// that state failed before. What a lookaround notes on the way to its match
// is forgotten, as is, after a match, what was noted at its end, where the
// next search may start. The result stays the same, and the time is the
// length of the input times the size of the regex, as in the bounded
// backtracker of Rust's regex crate. Only a regex with a backreference
// backtracks without bound, as in JavaScript.

#include "codegen/codegen.h"

#include "../../runtime/regex.h"

#include <array>
#include <map>
#include <set>

namespace tz {

namespace {

using llvm::BasicBlock;
using llvm::Value;

// The words of the instruction at `pc` (a repeat with its character).
uint32_t instructionSize(const uint32_t *code, uint32_t pc) {
  const uint32_t *op = code + pc;
  switch (op[0]) {
  case RE_CHAR:
  case RE_CHAR_BACK:
  case RE_GOTO:
  case RE_SPLIT_NEXT:
  case RE_SPLIT_GOTO:
  case RE_SET_POSITION:
    return 2;
  case RE_SET:
  case RE_SET_BACK:
    return 10 + 2 * op[1];
  case RE_SAVE:
  case RE_SAVE_BACK:
  case RE_RESET:
  case RE_SET_REGISTER:
  case RE_LOOK:
    return 3;
  case RE_LOOP:
    return 7;
  case RE_LOOP_END:
    return 5;
  case RE_REPEAT:
    return 5 + op[4];
  case RE_BACKREF:
    return 3 + op[2];
  default: // RE_MATCH, RE_FAIL, RE_ANY, RE_ALL and the assertions
    return 1;
  }
}

bool isBack(uint32_t op) {
  return op == RE_CHAR_BACK || op == RE_ANY_BACK || op == RE_ALL_BACK || op == RE_SET_BACK;
}

bool isCharInstruction(uint32_t op) {
  return op == RE_CHAR || op == RE_ANY || op == RE_ALL || op == RE_SET || isBack(op);
}

// A set of byte values.
using ByteSet = std::array<uint32_t, 8>;

bool hasByte(const ByteSet &s, uint32_t c) { return (s[c / 32] >> (c % 32)) & 1; }
void addByte(ByteSet &s, uint32_t c) { s[c / 32] |= 1u << (c % 32); }

// How often a byte comes in text, roughly per mille: the scan for where a
// match starts looks for the rarest units of the regex.
double byteFrequency(uint32_t c) {
  static const char order[] = "etaoinsrhldcumfpgwybvkxjqz";
  static const double lower[] = {100, 75, 70, 65, 63, 60, 56, 54, 45, 35, 33, 25, 24,
                                 21,  19, 17, 17, 16, 15, 13, 9,  6,  2,  1,  1,  1};
  if (c == ' ')
    return 150;
  if (c >= 'a' && c <= 'z')
    return lower[strchr(order, int(c)) - order];
  if (c >= 'A' && c <= 'Z')
    return lower[strchr(order, int(c | 0x20)) - order] / 10 + 1;
  if (c >= '0' && c <= '9')
    return 8;
  if (c == '\n' || c == ',' || c == '.')
    return 12;
  if (c > ' ' && c < 127)
    return 2;
  return 0.5;
}

// One vector compare of a scan: the bytes equal to `lo` or in lo..hi,
// after `| 0x20` when `fold` (both cases of letters at once).
struct ByteTest {
  uint32_t lo, hi;
  bool fold;
};

// The compares that tell a set's bytes, or none if it takes more than four.
std::vector<ByteTest> byteTests(ByteSet s) {
  std::vector<ByteTest> tests;
  // Letters in both cases: a range after `| 0x20`.
  for (uint32_t c = 'a'; c <= 'z';) {
    if (!hasByte(s, c) || !hasByte(s, c - 0x20)) {
      ++c;
      continue;
    }
    uint32_t end = c;
    while (end + 1 <= 'z' && hasByte(s, end + 1) && hasByte(s, end + 1 - 0x20))
      ++end;
    tests.push_back({c, end, true});
    for (uint32_t d = c; d <= end; ++d) {
      s[d / 32] &= ~(1u << (d % 32));
      s[(d - 0x20) / 32] &= ~(1u << ((d - 0x20) % 32));
    }
    c = end + 1;
  }
  for (uint32_t c = 0; c < 256;) {
    if (!hasByte(s, c)) {
      ++c;
      continue;
    }
    uint32_t end = c;
    while (end + 1 < 256 && hasByte(s, end + 1))
      ++end;
    if (end - c >= 2) {
      tests.push_back({c, end, false});
    } else {
      for (uint32_t d = c; d <= end; ++d)
        tests.push_back({d, d, false});
    }
    c = end + 1;
  }
  if (tests.size() > 4)
    tests.clear();
  return tests;
}

// The units a match can start with (see MatcherBuilder::unitsFirst).
struct FirstUnits {
  bool known = false;
  ByteSet bytes{};
  bool wide = false; // also some unit from 256 on
};

// The units a match can have at an offset from its start: the bytes, and
// whether units from 256 on too (in strings of two-byte units).
struct OffsetSet {
  ByteSet bytes{};
  bool wide = false;
};

// An offset the scan for where a match starts looks at.
struct ScanOffset {
  uint32_t offset;
  OffsetSet units;
};

// A loop with a counter (RE_LOOP), for the state the memo keeps.
struct CountedLoop {
  uint32_t pc, counter, min, max, body, exit, position;
  bool emptyCheck; // an iteration past `min` that matches nothing fails
};

// More memo rows than this, and the regex goes without the memo.
constexpr uint64_t kMaxMemoRows = 4096;
// More capture and register slots than this, and the interpreter runs the
// regex: the compiled code keeps them on the C stack.
constexpr uint32_t kMaxSlots = 4096;

class MatcherBuilder {
public:
  MatcherBuilder(llvm::Module &module, const uint32_t *program)
      : module_(module), ctx_(module.getContext()), b_(ctx_), program_(program),
        code_(program + program[TZ_RE_H_CODE]),
        codeSize_(program[TZ_RE_H_SOURCE] - program[TZ_RE_H_CODE]), flags_(program[TZ_RE_H_FLAGS]),
        captures_(program[TZ_RE_H_CAPTURES]), registers_(program[TZ_RE_H_REGISTERS]),
        unicode_((flags_ & (TZ_RE_UNICODE | TZ_RE_UNICODE_SETS)) != 0) {}

  // Null when the regex is too large for code of its own.
  llvm::Function *build();

private:
  // What the program is
  void analyze();
  bool inLoop(const CountedLoop &loop, uint32_t pc) const;
  bool flagged(const CountedLoop &loop, uint32_t pc) const;
  uint64_t contextSize(uint32_t pc) const;
  void planMemo();
  void planScan();
  std::vector<OffsetSet> offsetSets(unsigned limit) const;
  void closure(std::set<uint32_t> &states, bool &match, bool &unknown) const;

  // Emitting
  void emitWidth(bool wide);
  void emitHead();
  void emitInstruction(uint32_t pc);
  bool literalUnit(uint32_t pc, uint32_t &unit, uint32_t &fold) const;
  uint32_t literalRun(uint32_t pc) const;
  uint32_t emitLiteralRun(uint32_t pc, uint32_t run);
  void emitRepeat(uint32_t pc);
  void emitRepeatGreedy(uint32_t pc, bool memo);
  void emitRepeatLazy(uint32_t pc);
  uint32_t greedyLabel(uint32_t pc);
  uint32_t lazyLabel(uint32_t pc);
  Value *unitMatches(uint32_t atom, Value *u);
  FirstUnits unitsFirst(uint32_t pc) const;
  Value *startsWith(const FirstUnits &units, Value *pos);
  void emitLook(uint32_t pc);
  void emitLookSuccess(uint32_t look);
  void emitByteScan(Value *from, BasicBlock *found, llvm::AllocaInst *at, BasicBlock *none);
  Value *vectorIn(Value *v, const OffsetSet &units);
  Value *unitMask(Value *at, const OffsetSet &units);

  BasicBlock *newBlock(const llvm::Twine &name) { return BasicBlock::Create(ctx_, name, fn_); }
  BasicBlock *blockAt(uint32_t pc);
  // A variable of the matcher, in the entry block where mem2reg finds it.
  llvm::AllocaInst *entryVar(const llvm::Twine &name, llvm::Type *type = nullptr);
  Value *i32(uint32_t v) { return b_.getInt32(v); }
  Value *load(llvm::AllocaInst *var) { return b_.CreateLoad(var->getAllocatedType(), var); }
  void store(Value *v, llvm::AllocaInst *var) { b_.CreateStore(v, var); }
  bool open() { return b_.GetInsertBlock() && !b_.GetInsertBlock()->hasTerminator(); }
  // Goes on in a new block if `ok`, or to `fail` (the backtracking one by
  // default).
  void failUnless(Value *ok, BasicBlock *fail = nullptr);
  Value *unitAt(Value *index);
  Value *slotAddress(Value *index);
  Value *slotAddress(uint32_t index) { return slotAddress(i32(index)); }
  Value *loadSlot(uint32_t index) { return b_.CreateLoad(b_.getInt32Ty(), slotAddress(index)); }
  void setSlot(Value *index, Value *value);
  void setSlot(uint32_t index, Value *value) { setSlot(i32(index), value); }
  void reserve(uint32_t n);
  // Pushes an entry, in room reserve() made.
  void push(Value *code, Value *value);
  // Writes the stack entry at `index`, which is there.
  void putEntry(Value *index, Value *code, Value *value);
  // The entry below the one just popped, whose value a repeat keeps there.
  Value *dataValue();
  uint32_t resumeTo(BasicBlock *target);
  uint32_t newLabel(BasicBlock *block);
  void finish(Value *result);

  // A character at a position: the code point and the position past it (or
  // before it, back), or a jump to `fail`.
  std::pair<Value *, Value *> readChar(Value *pos, bool back, BasicBlock *fail);
  Value *charStep(uint32_t atom, Value *pos, BasicBlock *fail);
  Value *inSet(Value *c, const uint32_t *set, bool bytesOnly);
  Value *inByteSet(Value *c, const ByteSet &bits);
  llvm::Constant *byteTable(const ByteSet &bits);
  Value *isLineTerminator(Value *c);
  Value *isWordUnit(Value *u, bool folded);
  Value *isLead(Value *u);
  Value *isTrail(Value *u);
  Value *test(Value *cond, const std::function<Value *()> &value);

  // The memo
  Value *memoContext(uint32_t pc, Value *pos);
  void memoCheck(uint32_t row, uint32_t pc, Value *pos, BasicBlock *visited);
  void budgetCheck();
  void countWork(Value *from, Value *to);
  llvm::StructType *memoType();
  Value *memoLogLength();
  void withMemo(llvm::StringRef name, llvm::ArrayRef<Value *> args);

  llvm::Module &module_;
  llvm::LLVMContext &ctx_;
  llvm::IRBuilder<> b_;
  const uint32_t *program_;
  const uint32_t *code_;
  uint32_t codeSize_;
  uint32_t flags_, captures_, registers_;
  bool unicode_;

  std::vector<uint8_t> leader_;
  std::vector<int32_t> scope_; // the innermost lookaround around each pc, or -1
  std::map<uint32_t, uint32_t> lookOfMatch_;
  std::vector<CountedLoop> loops_;
  std::set<uint32_t> targets_;
  bool hasBackref_ = false, hasLook_ = false, hasLoops_ = false;
  bool anchored_ = false;
  // The memo: whether the regex has one, and its first row for each block
  // and each unbounded repeat.
  bool memo_ = false;
  uint32_t rows_ = 0;
  std::map<uint32_t, uint32_t> blockRow_, repeatRow_;
  // A greedy unbounded repeat of a class that the regex starts with, which
  // a failed attempt skips the run of: its pc, or -1.
  int64_t skipRepeat_ = -1;
  // The scan for where a match starts: up to three offsets and the units
  // there; `scanUnits_` if strings of two-byte units have it too (with u,
  // only where no character before the offsets can be a surrogate pair).
  std::vector<ScanOffset> scan_;
  bool scanUnits_ = false;
  bool scanEmpty_ = false; // some offset has no byte at all: no match

  llvm::Function *fn_ = nullptr;
  Value *units_ = nullptr, *length_ = nullptr, *start_ = nullptr, *caps_ = nullptr,
        *sticky_ = nullptr, *max_ = nullptr;
  llvm::AllocaInst *pos_ = nullptr, *top_ = nullptr, *base_ = nullptr, *capacity_ = nullptr,
                   *value_ = nullptr, *attempt_ = nullptr, *next_ = nullptr, *skip_ = nullptr,
                   *slots_ = nullptr, *result_ = nullptr, *steps_ = nullptr, *search_ = nullptr,
                   *memoAt_ = nullptr, *depth_ = nullptr, *scanAt_ = nullptr, *found_ = nullptr;
  std::map<uint32_t, std::array<llvm::AllocaInst *, 3>> lookVars_; // mark, position, log
  BasicBlock *exit_ = nullptr;
  llvm::GlobalVariable *stackVar_ = nullptr, *capacityVar_ = nullptr;
  std::map<ByteSet, llvm::Constant *> tables_;

  // For the width being emitted
  bool wide_ = false;
  llvm::Type *unitTy_ = nullptr;
  std::vector<BasicBlock *> blocks_;
  BasicBlock *fail_ = nullptr, *attemptFail_ = nullptr, *success_ = nullptr;
  llvm::SwitchInst *dispatch_ = nullptr;
  std::map<BasicBlock *, uint32_t> resumeIds_;
  std::map<uint32_t, uint32_t> repeatLabels_;
  uint32_t nextId_ = 1;
};

//===----------------------------------------------------------------------===//
// What the program is
//===----------------------------------------------------------------------===//

void MatcherBuilder::analyze() {
  leader_.assign(codeSize_ + 1, 0);
  scope_.assign(codeSize_ + 1, -1);
  leader_[0] = 1;
  std::vector<uint32_t> looks;
  for (uint32_t pc = 0; pc < codeSize_; pc += instructionSize(code_, pc)) {
    const uint32_t *op = code_ + pc;
    switch (op[0]) {
    case RE_GOTO:
    case RE_SPLIT_NEXT:
      leader_[op[1]] = 1;
      targets_.insert(op[1]);
      break;
    case RE_SPLIT_GOTO:
      leader_[op[1]] = leader_[pc + 2] = 1;
      targets_.insert(op[1]);
      targets_.insert(pc + 2);
      hasLoops_ = true;
      break;
    case RE_LOOP: {
      leader_[op[5]] = leader_[op[6]] = 1;
      targets_.insert(op[5]);
      targets_.insert(op[6]);
      const uint32_t *end = code_ + op[6] - 5; // its RE_LOOP_END
      loops_.push_back({pc, op[1], op[2], op[3], op[5], op[6], end[3], end[2] != RE_INFINITY});
      hasLoops_ = true;
      break;
    }
    case RE_LOOP_END:
      leader_[op[4]] = 1;
      targets_.insert(op[4]);
      break;
    case RE_REPEAT:
      leader_[pc + instructionSize(code_, pc)] = 1;
      if (op[2] > 1)
        hasLoops_ = true;
      break;
    case RE_LOOK:
      leader_[op[2]] = 1;
      targets_.insert(op[2]);
      lookOfMatch_[op[2] - 1] = pc;
      looks.push_back(pc);
      hasLook_ = true;
      break;
    case RE_BACKREF:
      hasBackref_ = true;
      break;
    default:
      break;
    }
    if (op[0] == RE_SPLIT_NEXT)
      hasLoops_ = true;
  }
  // Inner lookarounds come later and overwrite the outer ones.
  for (uint32_t look : looks)
    for (uint32_t pc = look + 3; pc < code_[look + 2]; ++pc)
      scope_[pc] = int32_t(look);

  const uint32_t *first = program_ + program_[TZ_RE_H_FIRST];
  anchored_ = (first[8] & TZ_RE_FIRST_ANCHORED) != 0;

  // The skip past a run: a greedy unbounded repeat of a class first, with
  // nothing before it but the start of a group.
  uint32_t pc = 0;
  while (pc < codeSize_ && code_[pc] == RE_SET_POSITION)
    pc += 2;
  if (pc < codeSize_ && code_[pc] == RE_REPEAT && code_[pc + 3] && code_[pc + 2] == RE_INFINITY &&
      !hasBackref_ && !isBack(code_[pc + 5]) && (pc == 0 || !targets_.count(pc)))
    skipRepeat_ = pc;
}

bool MatcherBuilder::inLoop(const CountedLoop &loop, uint32_t pc) const {
  return pc >= loop.pc && pc < loop.exit && scope_[loop.pc] == scope_[pc];
}

// Whether at `pc` it matters for `loop` if the iteration has matched
// anything yet: past the start of the body of a loop with the empty check.
bool MatcherBuilder::flagged(const CountedLoop &loop, uint32_t pc) const {
  return loop.emptyCheck && pc > loop.body;
}

uint64_t counterRange(const CountedLoop &loop) {
  return loop.max == RE_INFINITY ? uint64_t(loop.min) + 1 : uint64_t(loop.max) + 1;
}

// How many states the counted loops around `pc` can be in.
uint64_t MatcherBuilder::contextSize(uint32_t pc) const {
  uint64_t size = 1;
  for (const CountedLoop &loop : loops_) {
    if (!inLoop(loop, pc))
      continue;
    size *= counterRange(loop) * (flagged(loop, pc) ? 2 : 1);
    if (size > kMaxMemoRows)
      return size;
  }
  return size;
}

void MatcherBuilder::planMemo() {
  memo_ = !hasBackref_ && hasLoops_;
  if (!memo_)
    return;
  uint64_t rows = 0;
  for (uint32_t pc = 0; pc < codeSize_; pc += instructionSize(code_, pc)) {
    if (leader_[pc]) {
      blockRow_[pc] = uint32_t(rows);
      rows += contextSize(pc);
    }
    if (code_[pc] == RE_REPEAT && code_[pc + 2] == RE_INFINITY) {
      repeatRow_[pc] = uint32_t(rows);
      rows += contextSize(pc);
    }
    if (rows > kMaxMemoRows) {
      memo_ = false;
      return;
    }
  }
  rows_ = uint32_t(rows);
}

// Adds to `states` what the program gets to from them without reading a
// character: the states that read one, and whether the match may end
// (`match`) or what comes is not known (`unknown`, a backreference).
void MatcherBuilder::closure(std::set<uint32_t> &states, bool &match, bool &unknown) const {
  std::vector<uint32_t> work(states.begin(), states.end());
  std::set<uint32_t> seen(states.begin(), states.end());
  states.clear();
  auto add = [&](uint32_t pc) {
    if (seen.insert(pc).second)
      work.push_back(pc);
  };
  while (!work.empty()) {
    uint32_t pc = work.back();
    work.pop_back();
    const uint32_t *op = code_ + pc;
    uint32_t next = pc + instructionSize(code_, pc);
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
      if (op[1] == 0)
        add(next);
      break;
    case RE_BACKREF:
      unknown = true;
      break;
    default:
      if (isCharInstruction(op[0]))
        states.insert(pc);
      else
        add(next); // registers, captures, assertions
      break;
    }
  }
}

// The units a match can have at offsets 0, 1, ... of it, as long as every
// match is longer: for the scan for where one starts.
std::vector<OffsetSet> MatcherBuilder::offsetSets(unsigned limit) const {
  std::vector<OffsetSet> sets;
  std::set<uint32_t> states = {0};
  while (sets.size() < limit) {
    bool match = false, unknown = false;
    closure(states, match, unknown);
    if (match || unknown || states.empty())
      break;
    OffsetSet units;
    ByteSet &bytes = units.bytes;
    std::set<uint32_t> next;
    for (uint32_t pc : states) {
      const uint32_t *op = code_ + pc;
      uint32_t atom = pc, after = pc + instructionSize(code_, pc);
      if (op[0] == RE_REPEAT) {
        atom = pc + 5;
        if (op[2] > 1)
          next.insert(pc);
      }
      const uint32_t *a = code_ + atom;
      switch (a[0]) {
      case RE_CHAR:
        if (a[1] < 256)
          addByte(bytes, a[1]);
        else
          units.wide = true;
        break;
      case RE_ANY:
        for (uint32_t c = 0; c < 256; ++c)
          if (c != '\n' && c != '\r')
            addByte(bytes, c);
        units.wide = true;
        break;
      case RE_SET:
        for (int i = 0; i < 8; ++i)
          bytes[i] |= a[2 + i];
        if (a[1] > 0)
          units.wide = true;
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

void MatcherBuilder::planScan() {
  if (anchored_)
    return;
  std::vector<OffsetSet> sets = offsetSets(16);
  struct Choice {
    uint32_t offset;
    double share; // of the bytes of text in the set
  };
  std::vector<Choice> usable;
  for (uint32_t k = 0; k < sets.size(); ++k) {
    double share = 0;
    bool any = false;
    for (uint32_t c = 0; c < 256; ++c)
      if (hasByte(sets[k].bytes, c)) {
        share += byteFrequency(c);
        any = true;
      }
    if (!any)
      scanEmpty_ = true; // in a byte string nothing can come there
    if (any && !byteTests(sets[k].bytes).empty())
      usable.push_back({k, share / 1000});
  }
  std::stable_sort(usable.begin(), usable.end(),
                   [](const Choice &a, const Choice &b) { return a.share < b.share; });
  // The rarest offsets, up to three, until about one start in 256 is left.
  double share = 1;
  for (const Choice &choice : usable) {
    if (scan_.size() == 3 || (scan_.size() >= 2 && share < 1.0 / 256))
      break;
    share *= choice.share;
    scan_.push_back({choice.offset, sets[choice.offset]});
  }
  // Where nearly every byte may start a match, the scan by units is faster.
  if (share > 0.1)
    scan_.clear();
  uint32_t reach = 0;
  for (const ScanOffset &s : scan_)
    reach = std::max(reach, s.offset);
  scanUnits_ = !scan_.empty();
  for (uint32_t k = 0; unicode_ && k <= reach && k < sets.size(); ++k)
    scanUnits_ &= !sets[k].wide;
}

//===----------------------------------------------------------------------===//
// Emitting: helpers
//===----------------------------------------------------------------------===//

BasicBlock *MatcherBuilder::blockAt(uint32_t pc) {
  if (!blocks_[pc])
    blocks_[pc] = newBlock("pc" + llvm::Twine(pc));
  return blocks_[pc];
}

llvm::AllocaInst *MatcherBuilder::entryVar(const llvm::Twine &name, llvm::Type *type) {
  BasicBlock &entry = fn_->getEntryBlock();
  llvm::IRBuilder<> at(&entry, entry.begin());
  return at.CreateAlloca(type ? type : b_.getInt32Ty(), nullptr, name);
}

void MatcherBuilder::failUnless(Value *ok, BasicBlock *fail) {
  BasicBlock *next = newBlock("ok");
  b_.CreateCondBr(ok, next, fail ? fail : fail_);
  b_.SetInsertPoint(next);
}

Value *MatcherBuilder::unitAt(Value *index) {
  Value *address = b_.CreateInBoundsGEP(unitTy_, units_, b_.CreateZExt(index, b_.getInt64Ty()));
  return b_.CreateZExt(b_.CreateLoad(unitTy_, address), b_.getInt32Ty());
}

Value *MatcherBuilder::slotAddress(Value *index) {
  return b_.CreateInBoundsGEP(b_.getInt32Ty(), slots_, b_.CreateZExt(index, b_.getInt64Ty()));
}

// Sets a capture or register slot, with an undo record if a choice point
// may come back to the old value (with none on the stack, nothing can).
void MatcherBuilder::setSlot(Value *index, Value *value) {
  Value *address = slotAddress(index);
  Value *old = b_.CreateLoad(b_.getInt32Ty(), address);
  BasicBlock *record = newBlock("undo"), *write = newBlock("set"), *done = newBlock("set.done");
  b_.CreateCondBr(b_.CreateICmpNE(old, value), write, done);
  b_.SetInsertPoint(write);
  BasicBlock *store = newBlock("set.store");
  b_.CreateCondBr(b_.CreateICmpNE(load(top_), i32(0)), record, store);
  b_.SetInsertPoint(record);
  push(b_.CreateNot(index), old);
  b_.CreateBr(store);
  b_.SetInsertPoint(store);
  b_.CreateStore(value, address);
  b_.CreateBr(done);
  b_.SetInsertPoint(done);
}

// Room for `n` more entries on the stack: each instruction makes it for all
// it may push, so that a push needs no check of its own.
void MatcherBuilder::reserve(uint32_t n) {
  if (n == 0)
    return;
  Value *need = b_.CreateAdd(load(top_), i32(n));
  BasicBlock *grow = newBlock("grow"), *go = newBlock("room");
  b_.CreateCondBr(b_.CreateICmpUGT(need, load(capacity_)), grow, go);
  b_.SetInsertPoint(grow);
  llvm::FunctionCallee growFn = module_.getOrInsertFunction(
      "tz_re_bt_grow",
      llvm::FunctionType::get(b_.getPtrTy(), {b_.getInt32Ty(), b_.getInt32Ty()}, false));
  store(b_.CreateCall(growFn, {load(top_), need}), base_);
  store(b_.CreateLoad(b_.getInt32Ty(), capacityVar_), capacity_);
  b_.CreateBr(go);
  b_.SetInsertPoint(go);
}

void MatcherBuilder::push(Value *code, Value *value) {
  Value *top = load(top_);
  putEntry(top, code, value);
  store(b_.CreateAdd(top, i32(1)), top_);
}

void MatcherBuilder::putEntry(Value *index, Value *code, Value *value) {
  llvm::Type *i64 = b_.getInt64Ty();
  Value *entry = b_.CreateOr(b_.CreateZExt(code, i64),
                             b_.CreateShl(b_.CreateZExt(value, i64), b_.getInt64(32)));
  b_.CreateStore(entry, b_.CreateInBoundsGEP(i64, load(base_), b_.CreateZExt(index, i64)));
}

Value *MatcherBuilder::dataValue() {
  llvm::Type *i64 = b_.getInt64Ty();
  Value *below = b_.CreateSub(load(top_), i32(1));
  Value *entry =
      b_.CreateLoad(i64, b_.CreateInBoundsGEP(i64, load(base_), b_.CreateZExt(below, i64)));
  return b_.CreateTrunc(b_.CreateLShr(entry, b_.getInt64(32)), b_.getInt32Ty());
}

// A label for the stack whose block takes the popped value as the position
// and goes on at `target`.
uint32_t MatcherBuilder::resumeTo(BasicBlock *target) {
  auto it = resumeIds_.find(target);
  if (it != resumeIds_.end())
    return it->second;
  llvm::IRBuilderBase::InsertPointGuard guard(b_);
  BasicBlock *block = newBlock("resume");
  b_.SetInsertPoint(block);
  store(load(value_), pos_);
  b_.CreateBr(target);
  uint32_t id = newLabel(block);
  resumeIds_[target] = id;
  return id;
}

uint32_t MatcherBuilder::newLabel(BasicBlock *block) {
  uint32_t id = nextId_++;
  dispatch_->addCase(b_.getInt32(id), block);
  return id;
}

void MatcherBuilder::finish(Value *result) {
  store(result, result_);
  b_.CreateBr(exit_);
}

Value *MatcherBuilder::test(Value *cond, const std::function<Value *()> &value) {
  BasicBlock *from = b_.GetInsertBlock();
  BasicBlock *yes = newBlock("test"), *done = newBlock("test.done");
  b_.CreateCondBr(cond, yes, done);
  b_.SetInsertPoint(yes);
  Value *v = value();
  BasicBlock *end = b_.GetInsertBlock();
  b_.CreateBr(done);
  b_.SetInsertPoint(done);
  llvm::PHINode *phi = b_.CreatePHI(b_.getInt1Ty(), 2);
  phi->addIncoming(b_.getFalse(), from);
  phi->addIncoming(v, end);
  return phi;
}

Value *MatcherBuilder::isLead(Value *u) {
  return b_.CreateICmpULE(b_.CreateSub(u, i32(0xD800)), i32(0x3FF));
}

Value *MatcherBuilder::isTrail(Value *u) {
  return b_.CreateICmpULE(b_.CreateSub(u, i32(0xDC00)), i32(0x3FF));
}

Value *MatcherBuilder::isLineTerminator(Value *c) {
  Value *lt = b_.CreateOr(b_.CreateICmpEQ(c, i32('\n')), b_.CreateICmpEQ(c, i32('\r')));
  if (wide_)
    lt = b_.CreateOr(lt, b_.CreateICmpULE(b_.CreateSub(c, i32(0x2028)), i32(1)));
  return lt;
}

Value *MatcherBuilder::isWordUnit(Value *u, bool folded) {
  Value *letter = b_.CreateICmpULE(b_.CreateSub(b_.CreateOr(u, i32(0x20)), i32('a')), i32(25));
  Value *digit = b_.CreateICmpULE(b_.CreateSub(u, i32('0')), i32(9));
  Value *word = b_.CreateOr(b_.CreateOr(letter, digit), b_.CreateICmpEQ(u, i32('_')));
  if (folded && wide_)
    word = b_.CreateOr(
        word, b_.CreateOr(b_.CreateICmpEQ(u, i32(0x017F)), b_.CreateICmpEQ(u, i32(0x212A))));
  return word;
}

llvm::Constant *MatcherBuilder::byteTable(const ByteSet &bits) {
  llvm::Constant *&table = tables_[bits];
  if (table)
    return table;
  std::vector<uint8_t> bytes(256);
  for (uint32_t c = 0; c < 256; ++c)
    bytes[c] = hasByte(bits, c);
  auto *data = llvm::ConstantDataArray::get(ctx_, bytes);
  auto *global =
      new llvm::GlobalVariable(module_, data->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.class");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  table = global;
  return table;
}

// Whether `c` (below 256) is in `bits`: a few compares, or a table.
Value *MatcherBuilder::inByteSet(Value *c, const ByteSet &bits) {
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  for (uint32_t x = 0; x < 256;) {
    if (!hasByte(bits, x)) {
      ++x;
      continue;
    }
    uint32_t end = x;
    while (end + 1 < 256 && hasByte(bits, end + 1))
      ++end;
    runs.push_back({x, end});
    x = end + 1;
  }
  if (runs.empty())
    return b_.getFalse();
  if (runs.size() <= 3) {
    Value *in = nullptr;
    for (auto [lo, hi] : runs) {
      Value *one = lo == hi ? b_.CreateICmpEQ(c, i32(lo))
                            : b_.CreateICmpULE(b_.CreateSub(c, i32(lo)), i32(hi - lo));
      in = in ? b_.CreateOr(in, one) : one;
    }
    return in;
  }
  Value *index =
      b_.CreateZExt(b_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, c, i32(255)), b_.getInt64Ty());
  Value *entry =
      b_.CreateLoad(b_.getInt8Ty(), b_.CreateInBoundsGEP(b_.getInt8Ty(), byteTable(bits), index));
  Value *in = b_.CreateICmpNE(entry, b_.getInt8(0));
  return b_.CreateAnd(in, b_.CreateICmpULT(c, i32(256)));
}

// RE_SET's operands at `set`: n, 8 words of bits, n ranges from 256 on.
Value *MatcherBuilder::inSet(Value *c, const uint32_t *set, bool bytesOnly) {
  ByteSet bits;
  for (int i = 0; i < 8; ++i)
    bits[i] = set[1 + i];
  Value *in = inByteSet(c, bits);
  uint32_t n = set[0];
  if (bytesOnly || n == 0)
    return in;
  const uint32_t *ranges = set + 9;
  if (n <= 4) {
    for (uint32_t i = 0; i < n; ++i)
      in = b_.CreateOr(in, b_.CreateICmpULE(b_.CreateSub(c, i32(ranges[2 * i])),
                                            i32(ranges[2 * i + 1] - ranges[2 * i])));
    return in;
  }
  std::vector<uint32_t> words(ranges, ranges + 2 * n);
  auto *data = llvm::ConstantDataArray::get(ctx_, words);
  auto *global =
      new llvm::GlobalVariable(module_, data->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.ranges");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::FunctionCallee fn = module_.getOrInsertFunction(
      "tz_re_in_ranges",
      llvm::FunctionType::get(b_.getInt32Ty(), {b_.getInt32Ty(), b_.getPtrTy(), b_.getInt32Ty()},
                              false));
  // Below 256 the bits answer; the call is for the rest only.
  Value *high = test(b_.CreateICmpUGE(c, i32(256)), [&] {
    return b_.CreateICmpNE(b_.CreateCall(fn, {c, global, i32(n)}), i32(0));
  });
  return b_.CreateOr(in, high);
}

std::pair<Value *, Value *> MatcherBuilder::readChar(Value *pos, bool back, BasicBlock *fail) {
  if (!wide_ || !unicode_) {
    if (!back) {
      failUnless(b_.CreateICmpSLT(pos, length_), fail);
      return {unitAt(pos), b_.CreateAdd(pos, i32(1))};
    }
    failUnless(b_.CreateICmpSGT(pos, i32(0)), fail);
    Value *before = b_.CreateSub(pos, i32(1));
    return {unitAt(before), before};
  }
  // With u, a surrogate pair is one character, and in the middle of one there
  // is none (see read_forward in runtime/regex.c): the runtime reads it, as
  // a unit that is not a surrogate is the character itself.
  Value *at = pos;
  if (!back) {
    failUnless(b_.CreateICmpSLT(pos, length_), fail);
  } else {
    failUnless(b_.CreateICmpSGT(pos, i32(0)), fail);
    at = b_.CreateSub(pos, i32(1));
  }
  Value *c = unitAt(at);
  BasicBlock *single = b_.GetInsertBlock(), *surrogate = newBlock("surrogate"),
             *done = newBlock("char");
  Value *singlePos = back ? at : b_.CreateAdd(pos, i32(1));
  b_.CreateCondBr(b_.CreateICmpULE(b_.CreateSub(c, i32(0xD800)), i32(0x7FF)), surrogate, done);
  b_.SetInsertPoint(surrogate);
  llvm::Type *i64 = b_.getInt64Ty();
  llvm::FunctionCallee readFn = module_.getOrInsertFunction(
      "tz_re_read_pair",
      llvm::FunctionType::get(
          i64, {b_.getPtrTy(), b_.getInt32Ty(), b_.getInt32Ty(), b_.getInt32Ty()}, false));
  Value *read = b_.CreateCall(readFn, {units_, length_, pos, i32(back ? 1 : 0)});
  failUnless(b_.CreateICmpSGE(read, b_.getInt64(0)), fail);
  Value *code = b_.CreateTrunc(read, b_.getInt32Ty());
  Value *pairPos = b_.CreateTrunc(b_.CreateLShr(read, b_.getInt64(32)), b_.getInt32Ty());
  BasicBlock *slow = b_.GetInsertBlock();
  b_.CreateBr(done);
  b_.SetInsertPoint(done);
  llvm::PHINode *cp = b_.CreatePHI(b_.getInt32Ty(), 2), *np = b_.CreatePHI(b_.getInt32Ty(), 2);
  cp->addIncoming(c, single);
  cp->addIncoming(code, slow);
  np->addIncoming(singlePos, single);
  np->addIncoming(pairPos, slow);
  return {cp, np};
}

// The one-character instruction at `atom` at `pos`: the position past (or
// before) the character, or a jump to `fail`.
Value *MatcherBuilder::charStep(uint32_t atom, Value *pos, BasicBlock *fail) {
  const uint32_t *op = code_ + atom;
  bool back = isBack(op[0]);
  if (op[0] == RE_CHAR || op[0] == RE_CHAR_BACK) {
    uint32_t c = op[1];
    if (!wide_ && c > 255) {
      b_.CreateBr(fail ? fail : fail_);
      b_.SetInsertPoint(newBlock("never"));
      return pos;
    }
    if (!wide_ || !unicode_ || c < 0xD800 || (c > 0xDFFF && c < 0x10000)) {
      // A code unit: no pairs to think of.
      Value *at = pos;
      if (back) {
        failUnless(b_.CreateICmpSGT(pos, i32(0)), fail);
        at = b_.CreateSub(pos, i32(1));
      } else {
        failUnless(b_.CreateICmpSLT(pos, length_), fail);
      }
      failUnless(b_.CreateICmpEQ(unitAt(at), i32(c)), fail);
      return back ? at : b_.CreateAdd(pos, i32(1));
    }
    auto [cp, next] = readChar(pos, back, fail);
    failUnless(b_.CreateICmpEQ(cp, i32(c)), fail);
    return next;
  }
  auto [cp, next] = readChar(pos, back, fail);
  switch (op[0]) {
  case RE_ANY:
  case RE_ANY_BACK:
    failUnless(b_.CreateNot(isLineTerminator(cp)), fail);
    break;
  case RE_SET:
  case RE_SET_BACK:
    failUnless(inSet(cp, op + 1, !wide_), fail);
    break;
  default: // RE_ALL
    break;
  }
  return next;
}

//===----------------------------------------------------------------------===//
// The memo
//===----------------------------------------------------------------------===//

// Which state of the counted loops around `pc` the match is in.
Value *MatcherBuilder::memoContext(uint32_t pc, Value *pos) {
  Value *context = i32(0);
  for (const CountedLoop &loop : loops_) {
    if (!inLoop(loop, pc))
      continue;
    uint32_t range = uint32_t(counterRange(loop));
    Value *n = b_.CreateBinaryIntrinsic(llvm::Intrinsic::umin,
                                        loadSlot(2 * captures_ + loop.counter), i32(range - 1));
    context = b_.CreateAdd(b_.CreateMul(context, i32(range)), n);
    if (flagged(loop, pc)) {
      Value *matched = b_.CreateICmpNE(pos, loadSlot(2 * captures_ + loop.position));
      context =
          b_.CreateAdd(b_.CreateMul(context, i32(2)), b_.CreateZExt(matched, b_.getInt32Ty()));
    }
  }
  return context;
}

// The memo (TzReMemo in runtime/regex.h): its fields.
enum { kMemoPages, kMemoRows, kMemoLength, kMemoLog, kMemoLogLength, kMemoLogCapacity };

llvm::StructType *MatcherBuilder::memoType() {
  llvm::Type *ptr = b_.getPtrTy(), *i32Ty = b_.getInt32Ty();
  return llvm::StructType::get(ctx_, {ptr, i32Ty, i32Ty, ptr, i32Ty, i32Ty});
}

// Past the budget of work, the memo goes on. The work is the choices taken,
// the iterations of loops and the characters repeats read; the budget grows
// with the starts the search went past, so that a search that backtracks
// within bounds never needs the memo.
void MatcherBuilder::budgetCheck() {
  if (!memo_)
    return;
  llvm::Type *i64 = b_.getInt64Ty();
  Value *starts = b_.CreateSExt(b_.CreateSub(load(attempt_), load(search_)), i64);
  Value *budget = b_.CreateAdd(b_.CreateMul(starts, b_.getInt64(16)), b_.getInt64(1024));
  BasicBlock *over = newBlock("memo.over"), *start = newBlock("memo.start"),
             *go = newBlock("memo.go");
  b_.CreateCondBr(b_.CreateICmpSGE(b_.CreateSExt(load(steps_), i64), budget), over, go);
  b_.SetInsertPoint(over);
  store(i32(0x80000000u), steps_); // not again
  b_.CreateCondBr(b_.CreateIsNull(load(memoAt_)), start, go);
  b_.SetInsertPoint(start);
  llvm::FunctionCallee startFn = module_.getOrInsertFunction(
      "tz_re_memo_start",
      llvm::FunctionType::get(b_.getPtrTy(), {b_.getInt32Ty(), b_.getInt32Ty()}, false));
  store(b_.CreateCall(startFn, {i32(rows_), length_}), memoAt_);
  b_.CreateBr(go);
  b_.SetInsertPoint(go);
}

// Adds what a repeat read, from `from` to `to`, to the work.
void MatcherBuilder::countWork(Value *from, Value *to) {
  if (!memo_)
    return;
  Value *read = b_.CreateSub(to, from);
  read = b_.CreateSelect(b_.CreateICmpSLT(read, i32(0)), b_.CreateNeg(read), read);
  store(b_.CreateAdd(load(steps_), read), steps_);
}

// Notes that the match got to row `row` (and the loop state of `pc`) at
// `pos`, or goes to `visited` if it got there before. Only with the memo on.
void MatcherBuilder::memoCheck(uint32_t row, uint32_t pc, Value *pos, BasicBlock *visited) {
  Value *memo = load(memoAt_);
  BasicBlock *on = newBlock("memo"), *done = newBlock("memo.done");
  b_.CreateCondBr(b_.CreateIsNotNull(memo), on, done);
  b_.SetInsertPoint(on);
  // Inside a lookaround what is noted may lie on the way to its match,
  // which is no failure: noted down too, to be forgotten if it matches.
  Value *log =
      hasLook_ ? b_.CreateZExt(b_.CreateICmpNE(load(depth_), i32(0)), b_.getInt32Ty()) : i32(0);
  llvm::Type *i32Ty = b_.getInt32Ty();
  llvm::FunctionCallee visit = module_.getOrInsertFunction(
      "tz_re_memo_visit",
      llvm::FunctionType::get(i32Ty, {b_.getPtrTy(), i32Ty, i32Ty, i32Ty}, false));
  Value *seen =
      b_.CreateCall(visit, {memo, b_.CreateAdd(i32(row), memoContext(pc, pos)), pos, log});
  b_.CreateCondBr(b_.CreateICmpNE(seen, i32(0)), visited, done);
  b_.SetInsertPoint(done);
}

// How long the memo's log is (0 without the memo).
Value *MatcherBuilder::memoLogLength() {
  Value *memo = load(memoAt_);
  BasicBlock *from = b_.GetInsertBlock(), *on = newBlock("log"), *done = newBlock("log.done");
  b_.CreateCondBr(b_.CreateIsNotNull(memo), on, done);
  b_.SetInsertPoint(on);
  Value *length =
      b_.CreateLoad(b_.getInt32Ty(), b_.CreateStructGEP(memoType(), memo, kMemoLogLength));
  b_.CreateBr(done);
  b_.SetInsertPoint(done);
  llvm::PHINode *phi = b_.CreatePHI(b_.getInt32Ty(), 2);
  phi->addIncoming(i32(0), from);
  phi->addIncoming(length, on);
  return phi;
}

// Calls `name` with the memo first, if there is one.
void MatcherBuilder::withMemo(llvm::StringRef name, llvm::ArrayRef<Value *> args) {
  Value *memo = load(memoAt_);
  BasicBlock *on = newBlock("memo.call"), *done = newBlock("memo.called");
  b_.CreateCondBr(b_.CreateIsNotNull(memo), on, done);
  b_.SetInsertPoint(on);
  std::vector<llvm::Type *> types = {b_.getPtrTy()};
  std::vector<Value *> values = {memo};
  for (Value *arg : args) {
    types.push_back(arg->getType());
    values.push_back(arg);
  }
  llvm::FunctionCallee fn =
      module_.getOrInsertFunction(name, llvm::FunctionType::get(b_.getVoidTy(), types, false));
  b_.CreateCall(fn, values);
  b_.CreateBr(done);
  b_.SetInsertPoint(done);
}

//===----------------------------------------------------------------------===//
// Emitting the matcher
//===----------------------------------------------------------------------===//

llvm::Function *MatcherBuilder::build() {
  if (2 * captures_ + registers_ > kMaxSlots)
    return nullptr;
  analyze();
  planMemo();
  planScan();

  llvm::Type *ptr = b_.getPtrTy(), *i32Ty = b_.getInt32Ty();
  auto *type =
      llvm::FunctionType::get(i32Ty, {ptr, ptr, i32Ty, i32Ty, i32Ty, ptr, i32Ty, i32Ty}, false);
  fn_ = llvm::Function::Create(type, llvm::GlobalValue::PrivateLinkage, "tz.regex.match", module_);
  fn_->addFnAttr(llvm::Attribute::NoUnwind);
  fn_->setUWTableKind(llvm::UWTableKind::Default);
  units_ = fn_->getArg(1);
  Value *wide = fn_->getArg(2);
  length_ = fn_->getArg(3);
  start_ = fn_->getArg(4);
  caps_ = fn_->getArg(5);
  sticky_ = fn_->getArg(6);
  max_ = fn_->getArg(7);

  stackVar_ = module_.getNamedGlobal("tz_re_bt");
  if (!stackVar_)
    stackVar_ = new llvm::GlobalVariable(module_, ptr, false, llvm::GlobalValue::ExternalLinkage,
                                         nullptr, "tz_re_bt");
  capacityVar_ = module_.getNamedGlobal("tz_re_bt_capacity");
  if (!capacityVar_)
    capacityVar_ = new llvm::GlobalVariable(
        module_, i32Ty, false, llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_bt_capacity");

  BasicBlock *entry = newBlock("entry");
  b_.SetInsertPoint(entry);
  auto var = [&](llvm::Type *t, const char *name) { return b_.CreateAlloca(t, nullptr, name); };
  pos_ = var(i32Ty, "pos");
  top_ = var(i32Ty, "top");
  base_ = var(ptr, "stack");
  capacity_ = var(i32Ty, "capacity");
  value_ = var(i32Ty, "value");
  attempt_ = var(i32Ty, "attempt");
  next_ = var(i32Ty, "next");
  skip_ = var(i32Ty, "skip");
  result_ = var(i32Ty, "result");
  steps_ = var(i32Ty, "steps");
  search_ = var(i32Ty, "search");
  memoAt_ = var(ptr, "memo");
  depth_ = var(i32Ty, "depth");
  scanAt_ = var(i32Ty, "scan");
  found_ = var(i32Ty, "found");
  uint32_t slots = 2 * captures_ + registers_;
  slots_ = b_.CreateAlloca(llvm::ArrayType::get(i32Ty, slots), nullptr, "slots");
  for (uint32_t pc = 0; pc < codeSize_; pc += instructionSize(code_, pc))
    if (code_[pc] == RE_LOOK)
      lookVars_[pc] = {var(i32Ty, "look.mark"), var(i32Ty, "look.pos"), var(i32Ty, "look.log")};

  // Every slot holds a value from the start, so that undo records never read
  // undefined memory.
  b_.CreateMemSet(slots_, b_.getInt8(0xFF), slots * 4, llvm::MaybeAlign(4));
  store(b_.CreateLoad(ptr, stackVar_), base_);
  store(b_.CreateLoad(i32Ty, capacityVar_), capacity_);
  store(i32(0), top_);
  store(i32(0), depth_);
  store(llvm::ConstantPointerNull::get(b_.getPtrTy()), memoAt_);
  store(i32(0), steps_);
  store(i32(0), found_);
  exit_ = newBlock("exit");
  BasicBlock *bytes = newBlock("bytes"), *units = newBlock("units");
  b_.CreateCondBr(b_.CreateICmpNE(wide, i32(0)), units, bytes);

  b_.SetInsertPoint(bytes);
  emitWidth(false);
  b_.SetInsertPoint(units);
  emitWidth(true);

  b_.SetInsertPoint(exit_);
  withMemo("tz_re_memo_end", {});
  b_.CreateRet(load(result_));
  return fn_;
}

void MatcherBuilder::emitWidth(bool wide) {
  wide_ = wide;
  unitTy_ = wide ? b_.getInt16Ty() : b_.getInt8Ty();
  blocks_.assign(codeSize_ + 1, nullptr);
  resumeIds_.clear();
  repeatLabels_.clear();
  nextId_ = 1;

  // The backtracking: pop the stack, apply undo records, resume a choice.
  fail_ = newBlock("fail");
  attemptFail_ = newBlock("attempt.fail");
  success_ = newBlock("success");
  BasicBlock *startBlock = b_.GetInsertBlock();
  {
    b_.SetInsertPoint(fail_);
    Value *top = load(top_);
    BasicBlock *pop = newBlock("pop"), *undo = newBlock("undo.apply"),
               *resume = newBlock("dispatch");
    b_.CreateCondBr(b_.CreateICmpEQ(top, i32(0)), attemptFail_, pop);
    b_.SetInsertPoint(pop);
    Value *below = b_.CreateSub(top, i32(1));
    store(below, top_);
    llvm::Type *i64 = b_.getInt64Ty();
    Value *entry =
        b_.CreateLoad(i64, b_.CreateInBoundsGEP(i64, load(base_), b_.CreateZExt(below, i64)));
    Value *code = b_.CreateTrunc(entry, b_.getInt32Ty());
    Value *value = b_.CreateTrunc(b_.CreateLShr(entry, b_.getInt64(32)), b_.getInt32Ty());
    b_.CreateCondBr(b_.CreateICmpSLT(code, i32(0)), undo, resume);
    b_.SetInsertPoint(undo);
    b_.CreateStore(value, slotAddress(b_.CreateNot(code)));
    b_.CreateBr(fail_);
    b_.SetInsertPoint(resume);
    store(value, value_);
    if (memo_) {
      store(b_.CreateAdd(load(steps_), i32(1)), steps_);
      budgetCheck();
    }
    BasicBlock *never = newBlock("never");
    dispatch_ = b_.CreateSwitch(code, never);
    b_.SetInsertPoint(never);
    b_.CreateUnreachable();
  }

  b_.SetInsertPoint(startBlock);
  emitHead();

  // The code, block by block.
  for (uint32_t pc = 0; pc < codeSize_;) {
    if (leader_[pc]) {
      BasicBlock *block = blockAt(pc);
      if (open())
        b_.CreateBr(block);
      b_.SetInsertPoint(block);
      if (memo_) {
        memoCheck(blockRow_[pc], pc, load(pos_), fail_);
      }
    } else if (!open()) {
      pc += instructionSize(code_, pc); // nothing gets here
      continue;
    }
    if (uint32_t run = literalRun(pc); run >= 2) {
      pc = emitLiteralRun(pc, run);
      continue;
    }
    emitInstruction(pc);
    pc += instructionSize(code_, pc);
  }
  if (open())
    b_.CreateUnreachable();
}

// A unit that the instruction at `pc` matches, and the bits that may differ
// (0x20 for a letter of either case, under i), when it is a character of one
// code unit read forward: one of a run that a few wide compares can check.
bool MatcherBuilder::literalUnit(uint32_t pc, uint32_t &unit, uint32_t &fold) const {
  const uint32_t *op = code_ + pc;
  uint32_t limit = wide_ ? 0xFFFF : 0xFF;
  if (op[0] == RE_CHAR) {
    if (op[1] > limit || (wide_ && unicode_ && op[1] >= 0xD800 && op[1] <= 0xDFFF))
      return false;
    unit = op[1];
    fold = 0;
    return true;
  }
  if (op[0] != RE_SET || (wide_ && op[1] != 0))
    return false;
  // Exactly a letter in both cases.
  uint32_t count = 0, first = 0;
  for (uint32_t c = 0; c < 256; ++c)
    if ((op[2 + c / 32] >> (c % 32)) & 1) {
      if (count++ == 0)
        first = c;
    }
  uint32_t lower = first | 0x20;
  if (count != 2 || first != (lower & ~0x20u) || lower < 'a' || lower > 'z' ||
      !((op[2 + lower / 32] >> (lower % 32)) & 1))
    return false;
  unit = lower;
  fold = 0x20;
  return true;
}

// How many instructions from `pc` on are characters of one unit each, with
// no block starting among them.
uint32_t MatcherBuilder::literalRun(uint32_t pc) const {
  uint32_t run = 0, unit, fold;
  for (uint32_t at = pc; at < codeSize_ && literalUnit(at, unit, fold);
       at += instructionSize(code_, at)) {
    if (at != pc && leader_[at])
      break;
    ++run;
  }
  return run;
}

// The `run` characters from `pc` on: one check that they fit, then the
// units compared 8 bytes at a time. Returns the pc after them.
uint32_t MatcherBuilder::emitLiteralRun(uint32_t pc, uint32_t run) {
  std::vector<std::pair<uint32_t, uint32_t>> units; // unit, fold
  for (uint32_t i = 0; i < run; ++i) {
    uint32_t unit, fold;
    literalUnit(pc, unit, fold);
    units.push_back({unit, fold});
    pc += instructionSize(code_, pc);
  }
  llvm::Type *i64 = b_.getInt64Ty();
  Value *pos = load(pos_);
  Value *end = b_.CreateAdd(b_.CreateSExt(pos, i64), b_.getInt64(run));
  failUnless(b_.CreateICmpSLE(end, b_.CreateSExt(length_, i64)));
  uint32_t width = wide_ ? 2 : 1, done = 0;
  while (done < run) {
    uint32_t n = 1;
    while (n * 2 * width <= 8 && done + n * 2 <= run)
      n *= 2;
    uint64_t expected = 0, mask = 0;
    for (uint32_t k = 0; k < n; ++k) {
      expected |= uint64_t(units[done + k].first) << (8 * width * k);
      mask |= uint64_t(units[done + k].second) << (8 * width * k);
    }
    llvm::Type *chunk = b_.getIntNTy(8 * width * n);
    Value *address =
        b_.CreateInBoundsGEP(unitTy_, units_, b_.CreateZExt(b_.CreateAdd(pos, i32(done)), i64));
    Value *v = b_.CreateAlignedLoad(chunk, address, llvm::Align(1));
    if (mask)
      v = b_.CreateOr(v, llvm::ConstantInt::get(chunk, mask));
    failUnless(b_.CreateICmpEQ(v, llvm::ConstantInt::get(chunk, expected)));
    done += n;
  }
  store(b_.CreateAdd(pos, i32(run)), pos_);
  return pc;
}

// The loop over where a match may start, and what comes after an attempt.
void MatcherBuilder::emitHead() {
  const uint32_t *first = program_ + program_[TZ_RE_H_FIRST];
  bool filter = (first[8] & TZ_RE_FIRST_KNOWN) != 0;
  // A search starts here, and again after each match of a batch.
  store(start_, next_);
  BasicBlock *begin = newBlock("begin");
  b_.CreateBr(begin);
  b_.SetInsertPoint(begin);
  Value *start = load(next_);
  store(start, search_);
  store(i32(0), steps_);
  if (wide_ && unicode_) {
    // With u, a start inside a surrogate pair is the start of the pair.
    Value *inside =
        test(b_.CreateAnd(b_.CreateICmpSGT(start, i32(0)), b_.CreateICmpSLT(start, length_)), [&] {
          return b_.CreateAnd(isTrail(unitAt(start)), isLead(unitAt(b_.CreateSub(start, i32(1)))));
        });
    start = b_.CreateSelect(inside, b_.CreateSub(start, i32(1)), start);
  }
  store(start, next_);
  BasicBlock *head = newBlock("attempt.head");
  b_.CreateBr(head);

  b_.SetInsertPoint(head);
  Value *i = load(next_);
  BasicBlock *none = newBlock("none");
  if (anchored_)
    failUnless(b_.CreateICmpEQ(i, i32(0)), none);
  BasicBlock *attempt = newBlock("attempt");
  if (scanEmpty_ && !wide_) {
    b_.CreateBr(none);
  } else if (!anchored_ && (filter || (!scan_.empty() && (!wide_ || scanUnits_)))) {
    BasicBlock *scan = newBlock("scan");
    b_.CreateCondBr(b_.CreateICmpNE(sticky_, i32(0)), attempt, scan);
    b_.SetInsertPoint(scan);
    if (!scan_.empty() && (!wide_ || scanUnits_)) {
      emitByteScan(i, attempt, next_, none);
    } else {
      // Unit by unit, past those that cannot start a match.
      BasicBlock *loop = newBlock("scan.loop"), *body = newBlock("scan.unit"),
                 *found = newBlock("scan.found");
      store(i, scanAt_);
      b_.CreateBr(loop);
      b_.SetInsertPoint(loop);
      Value *at = load(scanAt_);
      b_.CreateCondBr(b_.CreateICmpSLT(at, length_), body, none);
      b_.SetInsertPoint(body);
      Value *u = unitAt(at);
      ByteSet bits;
      for (int k = 0; k < 8; ++k)
        bits[k] = first[k];
      Value *can = inByteSet(u, bits);
      if (wide_ && (first[8] & TZ_RE_FIRST_WIDE))
        can = b_.CreateOr(can, b_.CreateICmpUGE(u, i32(256)));
      BasicBlock *skip = newBlock("scan.next");
      b_.CreateCondBr(can, found, skip);
      b_.SetInsertPoint(skip);
      store(b_.CreateAdd(at, i32(1)), scanAt_);
      b_.CreateBr(loop);
      b_.SetInsertPoint(found);
      store(at, next_);
      b_.CreateBr(attempt);
    }
  } else {
    b_.CreateBr(attempt);
  }

  // An attempt: the captures undefined, the stack empty.
  b_.SetInsertPoint(attempt);
  i = load(next_);
  if (captures_ > 0)
    b_.CreateMemSet(slots_, b_.getInt8(0xFF), 8 * captures_, llvm::MaybeAlign(4));
  store(i32(0), top_);
  store(i, attempt_);
  store(i, skip_);
  store(i, pos_);
  budgetCheck();
  b_.CreateBr(blockAt(0));

  // No match from there: the next start, past a run the first repeat read.
  b_.SetInsertPoint(attemptFail_);
  BasicBlock *again = newBlock("attempt.next");
  b_.CreateCondBr(b_.CreateICmpNE(sticky_, i32(0)), none, again);
  b_.SetInsertPoint(again);
  Value *nextStart = b_.CreateAdd(load(attempt_), i32(1));
  bool skips = skipRepeat_ >= 0 && !(wide_ && unicode_ && code_[skipRepeat_ + 1] == 0);
  if (skips)
    nextStart = b_.CreateBinaryIntrinsic(llvm::Intrinsic::smax, nextStart,
                                         b_.CreateAdd(load(skip_), i32(1)));
  store(nextStart, next_);
  b_.CreateCondBr(b_.CreateICmpSGT(nextStart, length_), none, head);

  b_.SetInsertPoint(none);
  finish(load(found_));

  // A match: its slots after those of the matches before it in the batch.
  b_.SetInsertPoint(success_);
  Value *found = load(found_);
  Value *matchStart = load(attempt_), *matchEnd = load(pos_);
  Value *to =
      b_.CreateInBoundsGEP(b_.getInt32Ty(), caps_,
                           b_.CreateZExt(b_.CreateMul(found, i32(2 * captures_)), b_.getInt64Ty()));
  b_.CreateStore(matchStart, to);
  b_.CreateStore(matchEnd, b_.CreateConstInBoundsGEP1_32(b_.getInt32Ty(), to, 1));
  if (captures_ > 1)
    b_.CreateMemCpy(b_.CreateConstInBoundsGEP1_32(b_.getInt32Ty(), to, 2), llvm::MaybeAlign(4),
                    b_.CreateConstInBoundsGEP1_32(b_.getInt32Ty(), slots_, 2), llvm::MaybeAlign(4),
                    8 * (captures_ - 1));
  Value *counted = b_.CreateAdd(found, i32(1));
  store(counted, found_);
  BasicBlock *more = newBlock("batch.more"), *full = newBlock("batch.full");
  b_.CreateCondBr(b_.CreateICmpEQ(counted, max_), full, more);
  b_.SetInsertPoint(full);
  finish(counted);
  // The next search: from the end of this match, past it if it was empty
  // (AdvanceStringIndex: a whole surrogate pair with u).
  b_.SetInsertPoint(more);
  if (memo_) {
    // What the memo noted on the way to this match did not fail. The next
    // searches start at its end or later, and only lookarounds look back
    // (what they noted on the way to a match is forgotten anyway), so of
    // that only the position of the end can come again.
    withMemo("tz_re_memo_clear", {matchEnd});
  }
  Value *after = b_.CreateAdd(matchEnd, i32(1));
  if (wide_ && unicode_) {
    Value *pair = test(b_.CreateICmpSLT(after, length_), [&] {
      return b_.CreateAnd(isLead(unitAt(matchEnd)), isTrail(unitAt(after)));
    });
    after = b_.CreateSelect(pair, b_.CreateAdd(matchEnd, i32(2)), after);
  }
  Value *from = b_.CreateSelect(b_.CreateICmpEQ(matchStart, matchEnd), after, matchEnd);
  store(from, next_);
  BasicBlock *search = newBlock("batch.next");
  b_.CreateCondBr(b_.CreateICmpSGT(from, length_), full, search);
  b_.SetInsertPoint(search);
  b_.CreateBr(begin);
}

// Which units of a vector are in `units`, by the compares of byteTests.
Value *MatcherBuilder::vectorIn(Value *v, const OffsetSet &units) {
  auto *vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
  Value *folded = nullptr, *in = nullptr;
  if (wide_ && units.wide)
    in = b_.CreateICmpUGE(v, llvm::ConstantInt::get(vecTy, 256));
  for (const ByteTest &t : byteTests(units.bytes)) {
    Value *x = v;
    if (t.fold) {
      if (!folded)
        folded = b_.CreateOr(v, llvm::ConstantInt::get(vecTy, 0x20));
      x = folded;
    }
    Value *one = t.lo == t.hi
                     ? b_.CreateICmpEQ(x, llvm::ConstantInt::get(vecTy, t.lo))
                     : b_.CreateICmpULE(b_.CreateSub(x, llvm::ConstantInt::get(vecTy, t.lo)),
                                        llvm::ConstantInt::get(vecTy, t.hi - t.lo));
    in = in ? b_.CreateOr(in, one) : one;
  }
  return in;
}

// A bit for each unit of the 16 bytes from unit `at` on: which are in
// `units`.
Value *MatcherBuilder::unitMask(Value *at, const OffsetSet &units) {
  uint32_t lanes = wide_ ? 8 : 16;
  auto *vecTy = llvm::FixedVectorType::get(unitTy_, lanes);
  Value *address = b_.CreateInBoundsGEP(unitTy_, units_, b_.CreateZExt(at, b_.getInt64Ty()));
  Value *v = b_.CreateAlignedLoad(vecTy, address, llvm::Align(1));
  return b_.CreateZExt(b_.CreateBitCast(vectorIn(v, units), b_.getIntNTy(lanes)), b_.getInt32Ty());
}

// The scan for where a match may start: 32 bytes at a time for the units at
// the offsets of `scan_`, then 16, then unit by unit at the end. Stores the
// start found in `at` and goes to `found`, or to `none`.
void MatcherBuilder::emitByteScan(Value *from, BasicBlock *found, llvm::AllocaInst *at,
                                  BasicBlock *none) {
  llvm::Type *i64 = b_.getInt64Ty();
  uint32_t reach = 0;
  for (const ScanOffset &s : scan_)
    reach = std::max(reach, s.offset);
  uint32_t lanes = wide_ ? 8 : 16;
  auto candidates = [&](Value *i) {
    Value *mask = nullptr;
    for (const ScanOffset &s : scan_) {
      Value *one = unitMask(b_.CreateAdd(i, i32(s.offset)), s.units);
      mask = mask ? b_.CreateAnd(mask, one) : one;
    }
    return mask;
  };
  // Whether `width` more bytes and the offsets fit before the end.
  auto fits = [&](Value *i, uint32_t width) {
    Value *end = b_.CreateAdd(b_.CreateSExt(i, i64), b_.getInt64(reach + width));
    return b_.CreateICmpSLE(end, b_.CreateSExt(length_, i64));
  };
  store(from, scanAt_);
  BasicBlock *wideHead = newBlock("scan.32"), *wideBody = newBlock("scan.32.body"),
             *wideNext = newBlock("scan.32.next"), *vecHead = newBlock("scan.vec"),
             *vecBody = newBlock("scan.vec.body"), *vecNext = newBlock("scan.vec.next"),
             *hit = newBlock("scan.hit"), *tailHead = newBlock("scan.tail"),
             *tailBody = newBlock("scan.tail.body"), *tailNext = newBlock("scan.tail.next");
  llvm::AllocaInst *bitsVar = entryVar("scan.bits");
  b_.CreateBr(wideHead);

  b_.SetInsertPoint(wideHead);
  Value *i = load(scanAt_);
  b_.CreateCondBr(fits(i, 2 * lanes), wideBody, vecHead);
  b_.SetInsertPoint(wideBody);
  Value *low = candidates(i), *high = candidates(b_.CreateAdd(i, i32(lanes)));
  Value *bits = b_.CreateOr(low, b_.CreateShl(high, i32(lanes)));
  store(bits, bitsVar);
  b_.CreateCondBr(b_.CreateICmpNE(bits, i32(0)), hit, wideNext);
  b_.SetInsertPoint(wideNext);
  store(b_.CreateAdd(i, i32(2 * lanes)), scanAt_);
  b_.CreateBr(wideHead);

  b_.SetInsertPoint(vecHead);
  i = load(scanAt_);
  b_.CreateCondBr(fits(i, lanes), vecBody, tailHead);
  b_.SetInsertPoint(vecBody);
  bits = candidates(i);
  store(bits, bitsVar);
  b_.CreateCondBr(b_.CreateICmpNE(bits, i32(0)), hit, vecNext);
  b_.SetInsertPoint(vecNext);
  store(b_.CreateAdd(i, i32(lanes)), scanAt_);
  b_.CreateBr(vecHead);

  b_.SetInsertPoint(hit);
  Value *first = b_.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, load(bitsVar), b_.getTrue());
  store(b_.CreateAdd(load(scanAt_), first), at);
  b_.CreateBr(found);

  // The last bytes, one at a time: a match needs room for its offsets.
  b_.SetInsertPoint(tailHead);
  i = load(scanAt_);
  b_.CreateCondBr(b_.CreateICmpSLT(b_.CreateAdd(i, i32(reach)), length_), tailBody, none);
  b_.SetInsertPoint(tailBody);
  Value *in = nullptr;
  for (const ScanOffset &s : scan_) {
    Value *u = unitAt(b_.CreateAdd(i, i32(s.offset)));
    Value *one = inByteSet(u, s.units.bytes);
    if (wide_ && s.units.wide)
      one = b_.CreateOr(one, b_.CreateICmpUGE(u, i32(256)));
    in = in ? b_.CreateAnd(in, one) : one;
  }
  BasicBlock *tailHit = newBlock("scan.tail.hit");
  b_.CreateCondBr(in, tailHit, tailNext);
  b_.SetInsertPoint(tailHit);
  store(i, at);
  b_.CreateBr(found);
  b_.SetInsertPoint(tailNext);
  store(b_.CreateAdd(i, i32(1)), scanAt_);
  b_.CreateBr(tailHead);
}

// How many entries the code of the instruction at `pc` may push at most.
uint32_t pushesOf(const uint32_t *code, uint32_t pc) {
  const uint32_t *op = code + pc;
  switch (op[0]) {
  case RE_SPLIT_NEXT:
  case RE_SPLIT_GOTO:
  case RE_SET_POSITION:
  case RE_SET_REGISTER:
  case RE_LOOP:
  case RE_LOOP_END:
  case RE_LOOK:
    return 1;
  case RE_SAVE:
  case RE_SAVE_BACK:
  case RE_REPEAT:
    return 2;
  case RE_RESET:
    return 2 * (op[2] - op[1]);
  default:
    return 0;
  }
}

void MatcherBuilder::emitInstruction(uint32_t pc) {
  const uint32_t *op = code_ + pc;
  uint32_t slotBase = 2 * captures_;
  reserve(pushesOf(code_, pc));
  switch (op[0]) {
  case RE_MATCH: {
    auto it = lookOfMatch_.find(pc);
    if (it != lookOfMatch_.end())
      emitLookSuccess(it->second);
    else
      b_.CreateBr(success_);
    return;
  }
  case RE_FAIL:
    b_.CreateBr(fail_);
    return;
  case RE_CHAR:
  case RE_CHAR_BACK:
  case RE_ANY:
  case RE_ANY_BACK:
  case RE_ALL:
  case RE_ALL_BACK:
  case RE_SET:
  case RE_SET_BACK:
    store(charStep(pc, load(pos_), nullptr), pos_);
    return;
  case RE_LINE_START:
    failUnless(b_.CreateICmpEQ(load(pos_), i32(0)));
    return;
  case RE_LINE_START_M: {
    Value *pos = load(pos_);
    Value *after = test(b_.CreateICmpSGT(pos, i32(0)),
                        [&] { return isLineTerminator(unitAt(b_.CreateSub(pos, i32(1)))); });
    failUnless(b_.CreateOr(b_.CreateICmpEQ(pos, i32(0)), after));
    return;
  }
  case RE_LINE_END:
    failUnless(b_.CreateICmpEQ(load(pos_), length_));
    return;
  case RE_LINE_END_M: {
    Value *pos = load(pos_);
    Value *before =
        test(b_.CreateICmpSLT(pos, length_), [&] { return isLineTerminator(unitAt(pos)); });
    failUnless(b_.CreateOr(b_.CreateICmpEQ(pos, length_), before));
    return;
  }
  case RE_WORD_BOUNDARY:
  case RE_NOT_WORD_BOUNDARY:
  case RE_WORD_BOUNDARY_I:
  case RE_NOT_WORD_BOUNDARY_I: {
    bool folded = op[0] == RE_WORD_BOUNDARY_I || op[0] == RE_NOT_WORD_BOUNDARY_I;
    bool boundary = op[0] == RE_WORD_BOUNDARY || op[0] == RE_WORD_BOUNDARY_I;
    Value *pos = load(pos_);
    Value *a = test(b_.CreateICmpSGT(pos, i32(0)),
                    [&] { return isWordUnit(unitAt(b_.CreateSub(pos, i32(1))), folded); });
    Value *c =
        test(b_.CreateICmpSLT(pos, length_), [&] { return isWordUnit(unitAt(pos), folded); });
    Value *differ = b_.CreateXor(a, c);
    failUnless(boundary ? differ : b_.CreateNot(differ));
    return;
  }
  case RE_GOTO:
    if (op[1] <= pc && memo_)
      store(b_.CreateAdd(load(steps_), i32(1)), steps_); // an iteration: work
    b_.CreateBr(blockAt(op[1]));
    return;
  case RE_SPLIT_NEXT:
  case RE_SPLIT_GOTO: {
    // The way tried first and the one left for later, each with the
    // character it needs first if it is known: a way without its character
    // would fail at once, so it is not taken, nor is a choice point made.
    uint32_t first = op[0] == RE_SPLIT_NEXT ? pc + 2 : op[1];
    uint32_t later = op[0] == RE_SPLIT_NEXT ? op[1] : pc + 2;
    Value *pos = load(pos_);
    FirstUnits firstUnits = unitsFirst(first), laterUnits = unitsFirst(later);
    if (firstUnits.known) {
      BasicBlock *take = newBlock("split.first");
      b_.CreateCondBr(startsWith(firstUnits, pos), take, blockAt(later));
      b_.SetInsertPoint(take);
    }
    if (laterUnits.known) {
      BasicBlock *keep = newBlock("split.keep"), *go = newBlock("split.go");
      b_.CreateCondBr(startsWith(laterUnits, pos), keep, go);
      b_.SetInsertPoint(keep);
      push(i32(resumeTo(blockAt(later))), pos);
      b_.CreateBr(go);
      b_.SetInsertPoint(go);
    } else {
      push(i32(resumeTo(blockAt(later))), pos);
    }
    if (op[0] == RE_SPLIT_GOTO)
      b_.CreateBr(blockAt(first));
    return;
  }
  case RE_SET_POSITION:
    setSlot(slotBase + op[1], load(pos_));
    return;
  case RE_SAVE: {
    Value *from = loadSlot(slotBase + op[2]);
    setSlot(2 * op[1], from);
    setSlot(2 * op[1] + 1, load(pos_));
    return;
  }
  case RE_SAVE_BACK: {
    Value *to = loadSlot(slotBase + op[2]);
    setSlot(2 * op[1], load(pos_));
    setSlot(2 * op[1] + 1, to);
    return;
  }
  case RE_RESET: {
    if (op[2] - op[1] == 1) {
      setSlot(i32(2 * op[1]), i32(0xFFFFFFFFu));
      setSlot(i32(2 * op[1] + 1), i32(0xFFFFFFFFu));
      return;
    }
    // Slot by slot, in a loop.
    llvm::AllocaInst *k = entryVar("reset");
    store(i32(2 * op[1]), k);
    BasicBlock *loop = newBlock("reset.loop"), *body = newBlock("reset.slot"),
               *done = newBlock("reset.done");
    b_.CreateBr(loop);
    b_.SetInsertPoint(loop);
    Value *slot = load(k);
    b_.CreateCondBr(b_.CreateICmpULT(slot, i32(2 * op[2])), body, done);
    b_.SetInsertPoint(body);
    setSlot(slot, i32(0xFFFFFFFFu));
    store(b_.CreateAdd(slot, i32(1)), k);
    b_.CreateBr(loop);
    b_.SetInsertPoint(done);
    return;
  }
  case RE_SET_REGISTER:
    setSlot(slotBase + op[1], i32(op[2]));
    return;
  case RE_LOOP: {
    // counter, min, max, greedy, body, exit
    Value *n = loadSlot(slotBase + op[1]);
    BasicBlock *body = blockAt(op[5]), *exit = blockAt(op[6]);
    BasicBlock *more = newBlock("loop.more");
    b_.CreateCondBr(b_.CreateICmpULT(n, i32(op[2])), body, more);
    b_.SetInsertPoint(more);
    if (op[3] != RE_INFINITY) {
      BasicBlock *choose = newBlock("loop.choose");
      b_.CreateCondBr(b_.CreateICmpUGE(n, i32(op[3])), exit, choose);
      b_.SetInsertPoint(choose);
    }
    if (op[4]) {
      push(i32(resumeTo(exit)), load(pos_));
      b_.CreateBr(body);
    } else {
      push(i32(resumeTo(body)), load(pos_));
      b_.CreateBr(exit);
    }
    return;
  }
  case RE_LOOP_END: {
    // counter, min, position, loop: an optional iteration that matched
    // nothing fails.
    Value *n = loadSlot(slotBase + op[1]);
    if (op[2] != RE_INFINITY) {
      Value *empty = b_.CreateICmpEQ(load(pos_), loadSlot(slotBase + op[3]));
      failUnless(b_.CreateNot(b_.CreateAnd(b_.CreateICmpUGE(n, i32(op[2])), empty)));
    }
    Value *counted =
        b_.CreateSelect(b_.CreateICmpULT(n, i32(RE_INFINITY - 1)), b_.CreateAdd(n, i32(1)), n);
    setSlot(slotBase + op[1], counted);
    if (memo_)
      store(b_.CreateAdd(load(steps_), i32(1)), steps_);
    b_.CreateBr(blockAt(op[4]));
    return;
  }
  case RE_REPEAT:
    emitRepeat(pc);
    return;
  case RE_BACKREF: {
    std::vector<uint32_t> words(op, op + 3 + op[2]);
    auto *data = llvm::ConstantDataArray::get(ctx_, words);
    auto *global =
        new llvm::GlobalVariable(module_, data->getType(), /*isConstant=*/true,
                                 llvm::GlobalValue::PrivateLinkage, data, "tz.regex.backref");
    llvm::Type *ptr = b_.getPtrTy(), *i32Ty = b_.getInt32Ty();
    llvm::FunctionCallee fn = module_.getOrInsertFunction(
        "tz_re_backref",
        llvm::FunctionType::get(i32Ty, {ptr, i32Ty, i32Ty, ptr, i32Ty, ptr, i32Ty}, false));
    Value *next = b_.CreateCall(fn, {units_, i32(wide_ ? 1 : 0), length_, slots_, load(pos_),
                                     global, i32(unicode_ ? 1 : 0)});
    failUnless(b_.CreateICmpSGE(next, i32(0)));
    store(next, pos_);
    return;
  }
  case RE_LOOK:
    emitLook(pc);
    return;
  default:
    b_.CreateBr(fail_);
    return;
  }
}

//===----------------------------------------------------------------------===//
// Repeats of one character
//===----------------------------------------------------------------------===//

void MatcherBuilder::emitRepeat(uint32_t pc) {
  if (!code_[pc + 3]) {
    emitRepeatLazy(pc);
    return;
  }
  if (!memo_ || code_[pc + 2] != RE_INFINITY) {
    emitRepeatGreedy(pc, false);
    return;
  }
  // Without the memo as fast as it gets; with it, a note per position.
  BasicBlock *plain = newBlock("repeat.plain"), *noted = newBlock("repeat.memo");
  b_.CreateCondBr(b_.CreateIsNull(load(memoAt_)), plain, noted);
  b_.SetInsertPoint(plain);
  emitRepeatGreedy(pc, false);
  b_.SetInsertPoint(noted);
  emitRepeatGreedy(pc, true);
}

// The character that what follows a repeat must start with, past what only
// notes positions (as next_char in runtime/regex.c): -1 if none.
int64_t followingChar(const uint32_t *code, uint32_t pc, bool unicode) {
  for (;;) {
    const uint32_t *op = code + pc;
    switch (op[0]) {
    case RE_SET_POSITION:
      pc += 2;
      continue;
    case RE_SAVE:
    case RE_RESET:
      pc += 3;
      continue;
    case RE_CHAR:
      if (unicode && op[1] >= 0xD800 && (op[1] <= 0xDFFF || op[1] >= 0x10000))
        return -1;
      return op[1];
    default:
      return -1;
    }
  }
}

// The units that what the code at `pc` matches can start with, when every
// way from there reads a character forward before it can match: a split
// takes a way only if the unit at the position is one of them.
FirstUnits MatcherBuilder::unitsFirst(uint32_t pc) const {
  FirstUnits units;
  std::set<uint32_t> states = {pc};
  bool match = false, unknown = false;
  closure(states, match, unknown);
  if (match || unknown || states.empty())
    return units;
  for (uint32_t state : states) {
    const uint32_t *op = code_ + state;
    uint32_t atom = op[0] == RE_REPEAT ? state + 5 : state;
    const uint32_t *a = code_ + atom;
    switch (a[0]) {
    case RE_CHAR:
      if (a[1] < 256)
        addByte(units.bytes, a[1]);
      else
        units.wide = true;
      break;
    case RE_SET:
      for (int i = 0; i < 8; ++i)
        units.bytes[i] |= a[2 + i];
      if (a[1] > 0)
        units.wide = true;
      break;
    case RE_ANY:
    case RE_ALL:
      for (uint32_t c = 0; c < 256; ++c)
        if (a[0] == RE_ALL || (c != '\n' && c != '\r'))
          addByte(units.bytes, c);
      units.wide = true;
      break;
    default: // read backward: the unit at the position tells nothing
      return units;
    }
  }
  units.known = true;
  return units;
}

// Whether the unit at `pos` is one of `units` (or half of a surrogate pair
// with u, which may start a character of any code point).
Value *MatcherBuilder::startsWith(const FirstUnits &units, Value *pos) {
  return test(b_.CreateICmpSLT(pos, length_), [&] {
    Value *u = unitAt(pos);
    Value *in = inByteSet(u, units.bytes);
    if (wide_ && units.wide)
      in = b_.CreateOr(in, b_.CreateICmpUGE(u, i32(256)));
    else if (wide_ && unicode_)
      in = b_.CreateOr(in, b_.CreateICmpULE(b_.CreateSub(u, i32(0xD800)), i32(0x7FF)));
    return in;
  });
}

// Whether the character at `atom` matches the unit `u`, for the repeats that
// read by code units.
Value *MatcherBuilder::unitMatches(uint32_t atom, Value *u) {
  const uint32_t *op = code_ + atom;
  switch (op[0]) {
  case RE_CHAR:
    return b_.CreateICmpEQ(u, i32(op[1]));
  case RE_SET:
    return inSet(u, op + 1, !wide_);
  case RE_ANY:
    return b_.CreateNot(isLineTerminator(u));
  default: // RE_ALL
    return b_.getTrue();
  }
}

// A greedy repeat (min, max, greedy, length, the character): as many as
// match, then one less at a time, each on a choice point of its own made
// from one entry for all (the position where the repeat ends now) above one
// that keeps where `min` ended. With `memo` (and no upper bound) the read
// also stops at a position the repeat got to before: everything from there
// on failed then.
void MatcherBuilder::emitRepeatGreedy(uint32_t pc, bool memo) {
  const uint32_t *op = code_ + pc;
  uint32_t min = op[1], max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  bool back = isBack(code_[atom]);
  llvm::Type *i64 = b_.getInt64Ty();
  Value *start = load(pos_);
  llvm::AllocaInst *at = entryVar("repeat.at"), *count = entryVar("repeat.count");
  llvm::AllocaInst *minAt = entryVar("repeat.min"), *last = entryVar("repeat.last");
  store(start, at);
  store(i32(0), count);
  store(start, minAt);
  BasicBlock *done = newBlock("repeat.done");
  if (!memo && !back && (!wide_ || !unicode_)) {
    // By code units, up to the limit.
    Value *limit = length_;
    if (max != RE_INFINITY) {
      Value *bound = b_.CreateAdd(b_.CreateSExt(start, i64), b_.getInt64(max));
      limit = b_.CreateTrunc(
          b_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, bound, b_.CreateSExt(length_, i64)),
          b_.getInt32Ty());
    }
    BasicBlock *loop = newBlock("repeat.loop"), *read = newBlock("repeat.read"),
               *step = newBlock("repeat.step");
    b_.CreateBr(loop);
    b_.SetInsertPoint(loop);
    Value *p = load(at);
    b_.CreateCondBr(b_.CreateICmpSLT(p, limit), read, done);
    b_.SetInsertPoint(read);
    b_.CreateCondBr(unitMatches(atom, unitAt(p)), step, done);
    b_.SetInsertPoint(step);
    store(b_.CreateAdd(p, i32(1)), at);
    b_.CreateBr(loop);
    b_.SetInsertPoint(done);
    Value *end = load(at);
    if (int64_t(pc) == skipRepeat_)
      store(end, skip_);
    failUnless(b_.CreateICmpUGE(b_.CreateSub(end, start), i32(min)));
    store(b_.CreateAdd(start, i32(min)), minAt);
  } else if (!memo) {
    // Character by character.
    BasicBlock *loop = newBlock("repeat.loop"), *read = newBlock("repeat.read");
    b_.CreateBr(loop);
    b_.SetInsertPoint(loop);
    Value *n = load(count);
    if (max != RE_INFINITY)
      b_.CreateCondBr(b_.CreateICmpULT(n, i32(max)), read, done);
    else
      b_.CreateBr(read);
    b_.SetInsertPoint(read);
    Value *np = charStep(atom, load(at), done);
    store(np, at);
    Value *counted = b_.CreateAdd(n, i32(1));
    store(counted, count);
    if (min > 0) {
      BasicBlock *reached = newBlock("repeat.min");
      b_.CreateCondBr(b_.CreateICmpEQ(counted, i32(min)), reached, loop);
      b_.SetInsertPoint(reached);
      store(np, minAt);
    }
    b_.CreateBr(loop);
    b_.SetInsertPoint(done);
    if (int64_t(pc) == skipRepeat_)
      store(load(at), skip_);
    failUnless(b_.CreateICmpUGE(load(count), i32(min)));
  } else {
    // With the memo: `min` characters, then a note at each position.
    if (min > 0) {
      BasicBlock *loop = newBlock("repeat.first"), *read = newBlock("repeat.first.read");
      BasicBlock *reached = newBlock("repeat.first.done");
      b_.CreateBr(loop);
      b_.SetInsertPoint(loop);
      Value *n = load(count);
      b_.CreateCondBr(b_.CreateICmpULT(n, i32(min)), read, reached);
      b_.SetInsertPoint(read);
      store(charStep(atom, load(at), fail_), at);
      store(b_.CreateAdd(n, i32(1)), count);
      b_.CreateBr(loop);
      b_.SetInsertPoint(reached);
      store(load(at), minAt);
    }
    store(i32(0xFFFFFFFFu), last);
    BasicBlock *loop = newBlock("repeat.loop"), *seen = newBlock("repeat.seen");
    BasicBlock *exhausted = newBlock("repeat.end");
    b_.CreateBr(loop);
    b_.SetInsertPoint(loop);
    Value *p = load(at);
    memoCheck(repeatRow_[pc], pc, p, seen);
    store(p, last);
    store(charStep(atom, p, exhausted), at);
    b_.CreateBr(loop);
    // The read ends at the last position noted now; none: nothing to try.
    b_.SetInsertPoint(seen);
    b_.CreateBr(exhausted);
    b_.SetInsertPoint(exhausted);
    Value *end = load(last);
    failUnless(b_.CreateICmpNE(end, i32(0xFFFFFFFFu)));
    store(end, at);
    if (int64_t(pc) == skipRepeat_)
      store(end, skip_);
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
  }

  countWork(start, load(at));

  // Where what follows must start with a character, the repeat ends at the
  // last position before one at once: what follows would fail at the others
  // before it did anything (as the interpreter does on backtracking).
  int64_t hint = back ? -1 : followingChar(code_, after, unicode_);
  if (hint >= 0) {
    if (!wide_ && hint > 255) {
      b_.CreateBr(fail_);
      b_.SetInsertPoint(newBlock("never"));
    } else {
      BasicBlock *loop = newBlock("repeat.back"), *step = newBlock("repeat.back.step"),
                 *found = newBlock("repeat.back.done");
      Value *minPos = load(minAt);
      b_.CreateBr(loop);
      b_.SetInsertPoint(loop);
      Value *q = load(at);
      failUnless(b_.CreateICmpSGE(q, minPos));
      Value *there = test(b_.CreateICmpSLT(q, length_),
                          [&] { return b_.CreateICmpEQ(unitAt(q), i32(uint32_t(hint))); });
      b_.CreateCondBr(there, found, step);
      b_.SetInsertPoint(step);
      store(b_.CreateSub(q, i32(1)), at);
      b_.CreateBr(loop);
      b_.SetInsertPoint(found);
    }
  }

  // The choice point for fewer characters.
  Value *end = load(at), *minPos = load(minAt);
  BasicBlock *choice = newBlock("repeat.choice"), *go = newBlock("repeat.go");
  b_.CreateCondBr(b_.CreateICmpNE(end, minPos), choice, go);
  b_.SetInsertPoint(choice);
  push(i32(0), minPos);
  push(i32(greedyLabel(pc)), end);
  b_.CreateBr(go);
  b_.SetInsertPoint(go);
  store(end, pos_);
  b_.CreateBr(blockAt(after));
}

// The label of a greedy repeat's choice point, whose block takes one
// character less (the same with the memo and without: what follows notes
// its positions).
uint32_t MatcherBuilder::greedyLabel(uint32_t pc) {
  auto it = repeatLabels_.find(pc);
  if (it != repeatLabels_.end())
    return it->second;
  const uint32_t *op = code_ + pc;
  uint32_t atom = pc + 5, after = pc + 5 + op[4];
  bool back = isBack(code_[atom]);
  llvm::IRBuilderBase::InsertPointGuard guard(b_);
  BasicBlock *block = newBlock("repeat.less");
  uint32_t label = newLabel(block);
  repeatLabels_[pc] = label;
  b_.SetInsertPoint(block);
  Value *p = load(value_), *minPos = dataValue();
  BasicBlock *drop = newBlock("repeat.drop");
  Value *q;
  if (!back) {
    q = b_.CreateSub(p, i32(1));
    if (wide_ && unicode_) {
      // Back over a whole pair.
      Value *pair = test(b_.CreateICmpSGE(b_.CreateSub(p, i32(2)), minPos), [&] {
        return b_.CreateAnd(isTrail(unitAt(b_.CreateSub(p, i32(1)))),
                            isLead(unitAt(b_.CreateSub(p, i32(2)))));
      });
      q = b_.CreateSelect(pair, b_.CreateSub(p, i32(2)), q);
    }
    int64_t hint = followingChar(code_, after, unicode_);
    if (hint >= 0) {
      // Where the character that must follow is not, what follows would
      // fail: those positions are skipped.
      if (!wide_ && hint > 255) {
        b_.CreateBr(drop);
      } else {
        llvm::AllocaInst *at = entryVar("repeat.hint");
        store(q, at);
        BasicBlock *loop = newBlock("hint.loop"), *step = newBlock("hint.step"),
                   *found = newBlock("hint.done");
        b_.CreateBr(loop);
        b_.SetInsertPoint(loop);
        Value *x = load(at);
        Value *more = test(b_.CreateICmpSGT(x, minPos),
                           [&] { return b_.CreateICmpNE(unitAt(x), i32(uint32_t(hint))); });
        b_.CreateCondBr(more, step, found);
        b_.SetInsertPoint(step);
        store(b_.CreateSub(x, i32(1)), at);
        b_.CreateBr(loop);
        b_.SetInsertPoint(found);
        q = load(at);
        failUnless(b_.CreateICmpEQ(unitAt(q), i32(uint32_t(hint))), drop);
      }
    }
  } else {
    q = b_.CreateAdd(p, i32(1));
    if (wide_ && unicode_) {
      Value *pair = test(b_.CreateICmpSLE(b_.CreateAdd(p, i32(2)), minPos), [&] {
        return b_.CreateAnd(isLead(unitAt(p)), isTrail(unitAt(b_.CreateAdd(p, i32(1)))));
      });
      q = b_.CreateSelect(pair, b_.CreateAdd(p, i32(2)), q);
    }
  }
  if (open()) {
    // Fewer still next time, unless this is `min`.
    BasicBlock *keep = newBlock("repeat.keep"), *last = newBlock("repeat.last"),
               *go = newBlock("repeat.resume");
    b_.CreateCondBr(b_.CreateICmpNE(q, minPos), keep, last);
    b_.SetInsertPoint(keep);
    putEntry(load(top_), i32(label), q);
    store(b_.CreateAdd(load(top_), i32(1)), top_);
    b_.CreateBr(go);
    b_.SetInsertPoint(last);
    store(b_.CreateSub(load(top_), i32(1)), top_);
    b_.CreateBr(go);
    b_.SetInsertPoint(go);
    store(q, pos_);
    b_.CreateBr(blockAt(after));
  }
  b_.SetInsertPoint(drop);
  store(b_.CreateSub(load(top_), i32(1)), top_);
  b_.CreateBr(fail_);
  return label;
}

// A lazy repeat: `min` characters, then one more at a time, each on the
// choice point, which keeps the count in the entry below it. With the memo
// on (and no upper bound) a position the repeat got to before ends it.
void MatcherBuilder::emitRepeatLazy(uint32_t pc) {
  const uint32_t *op = code_ + pc;
  uint32_t min = op[1], max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  llvm::AllocaInst *at = entryVar("repeat.at"), *count = entryVar("repeat.count");
  store(load(pos_), at);
  store(i32(0), count);
  if (min > 0) {
    BasicBlock *loop = newBlock("lazy.first"), *read = newBlock("lazy.first.read");
    BasicBlock *reached = newBlock("lazy.first.done");
    b_.CreateBr(loop);
    b_.SetInsertPoint(loop);
    Value *n = load(count);
    b_.CreateCondBr(b_.CreateICmpULT(n, i32(min)), read, reached);
    b_.SetInsertPoint(read);
    store(charStep(atom, load(at), fail_), at);
    store(b_.CreateAdd(n, i32(1)), count);
    b_.CreateBr(loop);
    b_.SetInsertPoint(reached);
  }
  Value *p = load(at);
  if (memo_ && max == RE_INFINITY)
    memoCheck(repeatRow_[pc], pc, p, fail_);
  if (max > min) {
    push(i32(0), i32(min));
    push(i32(lazyLabel(pc)), p);
  }
  store(p, pos_);
  b_.CreateBr(blockAt(after));
}

uint32_t MatcherBuilder::lazyLabel(uint32_t pc) {
  auto it = repeatLabels_.find(pc);
  if (it != repeatLabels_.end())
    return it->second;
  const uint32_t *op = code_ + pc;
  uint32_t max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  llvm::IRBuilderBase::InsertPointGuard guard(b_);
  BasicBlock *block = newBlock("lazy.more");
  uint32_t label = newLabel(block);
  repeatLabels_[pc] = label;
  b_.SetInsertPoint(block);
  BasicBlock *drop = newBlock("lazy.drop");
  Value *count = dataValue();
  Value *np = charStep(atom, load(value_), drop);
  if (memo_ && max == RE_INFINITY)
    memoCheck(repeatRow_[pc], pc, np, drop);
  Value *counted = b_.CreateAdd(count, i32(1));
  BasicBlock *keep = newBlock("lazy.keep"), *last = newBlock("lazy.last"),
             *go = newBlock("lazy.resume");
  b_.CreateCondBr(b_.CreateICmpULT(counted, i32(max)), keep, last);
  b_.SetInsertPoint(keep);
  Value *top = load(top_);
  putEntry(b_.CreateSub(top, i32(1)), i32(0), counted);
  putEntry(top, i32(label), np);
  store(b_.CreateAdd(top, i32(1)), top_);
  b_.CreateBr(go);
  b_.SetInsertPoint(last);
  store(b_.CreateSub(load(top_), i32(1)), top_);
  b_.CreateBr(go);
  b_.SetInsertPoint(go);
  store(np, pos_);
  b_.CreateBr(blockAt(after));
  b_.SetInsertPoint(drop);
  store(b_.CreateSub(load(top_), i32(1)), top_);
  b_.CreateBr(fail_);
  return label;
}

//===----------------------------------------------------------------------===//
// Lookarounds
//===----------------------------------------------------------------------===//

// A lookaround (kind, next) runs its body right here, above a barrier on
// the stack whose label means "the body failed".
void MatcherBuilder::emitLook(uint32_t pc) {
  const uint32_t *op = code_ + pc;
  auto [mark, at, logMark] = lookVars_[pc];
  bool negative = (op[1] & RE_LOOK_NEGATIVE) != 0;
  Value *pos = load(pos_);
  store(pos, at);
  BasicBlock *failed = newBlock("look.failed");
  uint32_t label = newLabel(failed);
  push(i32(label), pos);
  store(b_.CreateSub(load(top_), i32(1)), mark);
  if (memo_ && hasLook_) {
    store(b_.CreateAdd(load(depth_), i32(1)), depth_);
    store(memoLogLength(), logMark);
  }
  {
    llvm::IRBuilderBase::InsertPointGuard guard(b_);
    b_.SetInsertPoint(failed);
    if (memo_ && hasLook_)
      store(b_.CreateSub(load(depth_), i32(1)), depth_);
    if (negative) {
      store(load(value_), pos_);
      b_.CreateBr(blockAt(op[2]));
    } else {
      b_.CreateBr(fail_);
    }
  }
}

// The end of a lookaround's body: it matched.
void MatcherBuilder::emitLookSuccess(uint32_t look) {
  const uint32_t *op = code_ + look;
  auto [mark, at, logMark] = lookVars_[look];
  bool negative = (op[1] & RE_LOOK_NEGATIVE) != 0;
  llvm::Type *ptr = b_.getPtrTy(), *i32Ty = b_.getInt32Ty();
  if (memo_ && hasLook_) {
    store(b_.CreateSub(load(depth_), i32(1)), depth_);
    // What the memo noted on the way may have led here: forgotten.
    withMemo("tz_re_memo_forget", {load(logMark)});
  }
  if (!negative) {
    llvm::FunctionCallee drop = module_.getOrInsertFunction(
        "tz_re_bt_drop_choices", llvm::FunctionType::get(i32Ty, {ptr, i32Ty, i32Ty}, false));
    store(b_.CreateCall(drop, {load(base_), load(mark), load(top_)}), top_);
    store(load(at), pos_);
    b_.CreateBr(blockAt(op[2]));
  } else {
    llvm::FunctionCallee unwind = module_.getOrInsertFunction(
        "tz_re_bt_unwind",
        llvm::FunctionType::get(b_.getVoidTy(), {ptr, i32Ty, i32Ty, ptr}, false));
    Value *m = load(mark);
    b_.CreateCall(unwind, {load(base_), m, load(top_), slots_});
    store(m, top_);
    b_.CreateBr(fail_);
  }
}

} // namespace

llvm::Constant *CodeGen::regexMatcher(const RegexLitExpr *lit) {
  llvm::Constant *&matcher = regexMatchers_[lit->program];
  if (matcher)
    return matcher;
  MatcherBuilder builder(*module_, lit->program.data());
  matcher = builder.build();
  if (!matcher)
    matcher = cast<llvm::Function>(
        regexFunction("tz_re_interpret", builder_.getInt32Ty(),
                      {builder_.getPtrTy(), builder_.getPtrTy(), builder_.getInt32Ty(),
                       builder_.getInt32Ty(), builder_.getInt32Ty(), builder_.getPtrTy(),
                       builder_.getInt32Ty(), builder_.getInt32Ty()})
            .getCallee());
  return matcher;
}

} // namespace tz
