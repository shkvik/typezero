#pragma once

#include <memory>
#include "compiler/lower/codegen/codegen.h"
#include <array>
#include <map>
#include <set>

namespace tz {

// The machine code of one regular expression, built from its program.

// The words of the instruction at `pc` (a repeat with its character).
uint32_t instructionSize(const uint32_t *code, uint32_t pc);

// A set of byte values.
using ByteSet = std::array<uint32_t, 8>;

// How often a byte comes in text, roughly per mille: the scan for where a
// match starts looks for the rarest units of the regex.
double byteFrequency(uint32_t c);

// One vector compare of a scan: the bytes equal to `lo` or in lo..hi,
// after `| 0x20` when `fold` (both cases of letters at once).
struct ByteTest {
  uint32_t lo, hi;
  bool fold;
};

// The compares that tell a set's bytes, or none if it takes more than four.
std::vector<ByteTest> byteTests(ByteSet s);

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
inline constexpr uint64_t kMaxMemoRows = 4096;

// More capture and register slots than this, and the interpreter runs the
// regex: the compiled code keeps them on the C stack.
inline constexpr uint32_t kMaxSlots = 4096;

class MatcherAnalysis;
class MatcherChars;
class MatcherMemo;
class MatcherHead;
class MatcherInstructions;
class MatcherRepeats;

class MatcherBuilder {
public:
  // `threadLocal`: the stack of backtracking is each thread's own (in a
  // program of the parallel runtime, runtime/library/regex/regex/regex.c).
  MatcherBuilder(llvm::Module &module, const uint32_t *program, bool threadLocal);
  // Null when the regex is too large for code of its own.
  llvm::Function *build();
private:
  llvm::BasicBlock *newBlock(const llvm::Twine &name);
  llvm::BasicBlock *blockAt(uint32_t pc);
  // A variable of the matcher, in the entry block where mem2reg finds it.
  llvm::AllocaInst *entryVar(const llvm::Twine &name, llvm::Type *type = nullptr);
  llvm::Value *i32(uint32_t v);
  llvm::Value *load(llvm::AllocaInst *var);
  void store(llvm::Value *v, llvm::AllocaInst *var);
  bool open();
  // Goes on in a new block if `ok`, or to `fail` (the backtracking one by
  // default).
  void failUnless(llvm::Value *ok, llvm::BasicBlock *fail = nullptr);
  llvm::Value *unitAt(llvm::Value *index);
  llvm::Value *slotAddress(llvm::Value *index);
  llvm::Value *slotAddress(uint32_t index);
  llvm::Value *loadSlot(uint32_t index);
  void setSlot(llvm::Value *index, llvm::Value *value);
  void setSlot(uint32_t index, llvm::Value *value);
  void reserve(uint32_t n);
  // Pushes an entry, in room reserve() made.
  void push(llvm::Value *code, llvm::Value *value);
  // Writes the stack entry at `index`, which is there.
  void putEntry(llvm::Value *index, llvm::Value *code, llvm::Value *value);
  // The entry below the one just popped, whose value a repeat keeps there.
  llvm::Value *dataValue();
  uint32_t resumeTo(llvm::BasicBlock *target);
  uint32_t newLabel(llvm::BasicBlock *block);
  void finish(llvm::Value *result);
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
  llvm::Value *units_ = nullptr, *length_ = nullptr, *start_ = nullptr, *caps_ = nullptr,
        *sticky_ = nullptr, *max_ = nullptr;
  llvm::AllocaInst *pos_ = nullptr, *top_ = nullptr, *base_ = nullptr, *capacity_ = nullptr,
                   *value_ = nullptr, *attempt_ = nullptr, *next_ = nullptr, *skip_ = nullptr,
                   *slots_ = nullptr, *result_ = nullptr, *steps_ = nullptr, *search_ = nullptr,
                   *memoAt_ = nullptr, *depth_ = nullptr, *scanAt_ = nullptr, *found_ = nullptr;
  std::map<uint32_t, std::array<llvm::AllocaInst *, 3>> lookVars_; // mark, position, log
  llvm::BasicBlock *exit_ = nullptr;
  llvm::GlobalVariable *stackVar_ = nullptr, *capacityVar_ = nullptr;
  bool threadLocal_;
  // The address of one of them, for this thread.
  llvm::Value *here(llvm::GlobalVariable *var);
  std::map<ByteSet, llvm::Constant *> tables_;
  // For the width being emitted
  bool wide_ = false;
  llvm::Type *unitTy_ = nullptr;
  std::vector<llvm::BasicBlock *> blocks_;
  llvm::BasicBlock *fail_ = nullptr, *attemptFail_ = nullptr, *success_ = nullptr;
  llvm::SwitchInst *dispatch_ = nullptr;
  std::map<llvm::BasicBlock *, uint32_t> resumeIds_;
  std::map<uint32_t, uint32_t> repeatLabels_;
  uint32_t nextId_ = 1;

public:
  ~MatcherBuilder();

  MatcherAnalysis &matcherAnalysis() const { return *matcherAnalysisPart_; }
  MatcherChars &matcherChars() const { return *matcherCharsPart_; }
  MatcherMemo &matcherMemo() const { return *matcherMemoPart_; }
  MatcherHead &matcherHead() const { return *matcherHeadPart_; }
  MatcherInstructions &matcherInstructions() const { return *matcherInstructionsPart_; }
  MatcherRepeats &matcherRepeats() const { return *matcherRepeatsPart_; }

private:
  friend class MatcherAnalysis;
  friend class MatcherChars;
  friend class MatcherMemo;
  friend class MatcherHead;
  friend class MatcherInstructions;
  friend class MatcherRepeats;
  std::unique_ptr<MatcherAnalysis> matcherAnalysisPart_;
  std::unique_ptr<MatcherChars> matcherCharsPart_;
  std::unique_ptr<MatcherMemo> matcherMemoPart_;
  std::unique_ptr<MatcherHead> matcherHeadPart_;
  std::unique_ptr<MatcherInstructions> matcherInstructionsPart_;
  std::unique_ptr<MatcherRepeats> matcherRepeatsPart_;
};

// The memo (TzReMemo in runtime/library/regex/regex/regex.h): its fields.
inline enum { kMemoPages, kMemoRows, kMemoLength, kMemoLog, kMemoLogLength, kMemoLogCapacity };

// How many entries the code of the instruction at `pc` may push at most.
uint32_t pushesOf(const uint32_t *code, uint32_t pc);

// The character that what follows a repeat must start with, past what only
// notes positions (as next_char in runtime/library/regex/regex/regex.c): -1 if
// none.
int64_t followingChar(const uint32_t *code, uint32_t pc, bool unicode);

} // namespace tz
