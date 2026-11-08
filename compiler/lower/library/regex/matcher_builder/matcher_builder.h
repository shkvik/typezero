#pragma once

#include "compiler/lower/lowering/lowering.h"
#include <array>
#include <map>
#include <memory>
#include <set>

namespace tz {

using ByteSet = std::array<uint32_t, 8>;

struct ByteTest {
  uint32_t lo, hi;
  bool fold;
};

struct FirstUnits {
  bool known = false;
  ByteSet bytes{};
  bool wide = false;
};

struct OffsetSet {
  ByteSet bytes{};
  bool wide = false;
};

struct ScanOffset {
  uint32_t offset;
  OffsetSet units;
};

struct CountedLoop {
  uint32_t pc, counter, min, max, body, exit, position;
  bool emptyCheck;
};

inline constexpr uint64_t kMaxMemoRows = 4096;

inline constexpr uint32_t kMaxSlots = 4096;

class MatcherAnalysis;
class MatcherChars;
class MatcherMemo;
class MatcherHead;
class MatcherInstructions;
class MatcherRepeats;

class MatcherBuilder {
public:
  static bool isBack(uint32_t op);
  static bool isCharInstruction(uint32_t op);
  static void addByte(ByteSet &s, uint32_t c);
  static bool hasByte(const ByteSet &s, uint32_t c);
  static uint64_t counterRange(const CountedLoop &loop);
  static uint32_t instructionSize(const uint32_t *code, uint32_t pc);
  static double byteFrequency(uint32_t c);
  static std::vector<ByteTest> byteTests(ByteSet s);
  static uint32_t pushesOf(const uint32_t *code, uint32_t pc);
  static int64_t followingChar(const uint32_t *code, uint32_t pc, bool unicode);

  MatcherBuilder(llvm::Module &module, const uint32_t *program, bool threadLocal);
  llvm::Function *build();

private:
  llvm::BasicBlock *newBlock(const llvm::Twine &name);
  llvm::BasicBlock *blockAt(uint32_t pc);
  llvm::AllocaInst *entryVar(const llvm::Twine &name, llvm::Type *type = nullptr);
  llvm::Value *i32(uint32_t v);
  llvm::Value *load(llvm::AllocaInst *var);
  void store(llvm::Value *v, llvm::AllocaInst *var);
  bool open();
  void failUnless(llvm::Value *ok, llvm::BasicBlock *failBlock = nullptr);
  llvm::Value *unitAt(llvm::Value *index);
  llvm::Value *slotAddress(llvm::Value *index);
  llvm::Value *slotAddress(uint32_t index);
  llvm::Value *loadSlot(uint32_t index);
  void setSlot(llvm::Value *index, llvm::Value *slotValue);
  void setSlot(uint32_t index, llvm::Value *slotValue);
  void reserve(uint32_t n);
  void push(llvm::Value *pushedCode, llvm::Value *pushedValue);
  void putEntry(llvm::Value *index, llvm::Value *entryCode, llvm::Value *entryValue);
  llvm::Value *dataValue();
  uint32_t resumeTo(llvm::BasicBlock *target);
  uint32_t newLabel(llvm::BasicBlock *block);
  void finish(llvm::Value *outcome);
  llvm::Module &module_;
  llvm::LLVMContext &ctx;
  llvm::IRBuilder<> b;
  const uint32_t *program_;
  const uint32_t *code;
  uint32_t codeSize;
  uint32_t flags, captures, registers;
  bool unicode;
  std::vector<uint8_t> leader;
  std::vector<int32_t> scope;
  std::map<uint32_t, uint32_t> lookOfMatch;
  std::vector<CountedLoop> loops;
  std::set<uint32_t> targets;
  bool hasBackref = false, hasLook = false, hasLoops = false;
  bool anchored = false;
  bool memo = false;
  uint32_t rows = 0;
  std::map<uint32_t, uint32_t> blockRow, repeatRow;
  int64_t skipRepeat = -1;
  std::vector<ScanOffset> scan;
  bool scanUnits = false;
  bool scanEmpty = false;
  llvm::Function *fn = nullptr;
  llvm::Value *units = nullptr, *length = nullptr, *start = nullptr, *caps = nullptr,
              *sticky = nullptr, *max = nullptr;
  llvm::AllocaInst *pos = nullptr, *top = nullptr, *base = nullptr, *capacity = nullptr,
                   *value = nullptr, *attempt = nullptr, *next = nullptr, *skip = nullptr,
                   *slots = nullptr, *result = nullptr, *steps = nullptr, *search = nullptr,
                   *memoAt = nullptr, *depth = nullptr, *scanAt = nullptr, *found = nullptr;
  std::map<uint32_t, std::array<llvm::AllocaInst *, 3>> lookVars;
  llvm::BasicBlock *exit = nullptr;
  llvm::GlobalVariable *stackVar = nullptr, *capacityVar = nullptr;
  bool threadLocal;
  llvm::Value *here(llvm::GlobalVariable *var);
  std::map<ByteSet, llvm::Constant *> tables;
  bool wide = false;
  llvm::Type *unitTy = nullptr;
  std::vector<llvm::BasicBlock *> blocks;
  llvm::BasicBlock *fail = nullptr, *attemptFail = nullptr, *success = nullptr;
  llvm::SwitchInst *dispatch = nullptr;
  std::map<llvm::BasicBlock *, uint32_t> resumeIds;
  std::map<uint32_t, uint32_t> repeatLabels;
  uint32_t nextId = 1;

public:
  ~MatcherBuilder();

  MatcherAnalysis &matcherAnalysis() const {
    return *this->matcherAnalysisPart;
  }
  MatcherChars &matcherChars() const {
    return *this->matcherCharsPart;
  }
  MatcherMemo &matcherMemo() const {
    return *this->matcherMemoPart;
  }
  MatcherHead &matcherHead() const {
    return *this->matcherHeadPart;
  }
  MatcherInstructions &matcherInstructions() const {
    return *this->matcherInstructionsPart;
  }
  MatcherRepeats &matcherRepeats() const {
    return *this->matcherRepeatsPart;
  }

private:
  friend class MatcherAnalysis;
  friend class MatcherChars;
  friend class MatcherMemo;
  friend class MatcherHead;
  friend class MatcherInstructions;
  friend class MatcherRepeats;
  std::unique_ptr<MatcherAnalysis> matcherAnalysisPart;
  std::unique_ptr<MatcherChars> matcherCharsPart;
  std::unique_ptr<MatcherMemo> matcherMemoPart;
  std::unique_ptr<MatcherHead> matcherHeadPart;
  std::unique_ptr<MatcherInstructions> matcherInstructionsPart;
  std::unique_ptr<MatcherRepeats> matcherRepeatsPart;
};

enum { kMemoPages, kMemoRows, kMemoLength, kMemoLog, kMemoLogLength, kMemoLogCapacity };

}
