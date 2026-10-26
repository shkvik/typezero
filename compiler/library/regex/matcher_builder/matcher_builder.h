#pragma once

#include "compiler/lower/lowering/lowering.h"
#include <array>
#include <map>
#include <memory>
#include <set>

namespace tz {

uint32_t instructionSize(const uint32_t *code, uint32_t pc);

using ByteSet = std::array<uint32_t, 8>;

double byteFrequency(uint32_t c);

struct ByteTest {
  uint32_t lo, hi;
  bool fold;
};

std::vector<ByteTest> byteTests(ByteSet s);

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
  void failUnless(llvm::Value *ok, llvm::BasicBlock *fail = nullptr);
  llvm::Value *unitAt(llvm::Value *index);
  llvm::Value *slotAddress(llvm::Value *index);
  llvm::Value *slotAddress(uint32_t index);
  llvm::Value *loadSlot(uint32_t index);
  void setSlot(llvm::Value *index, llvm::Value *value);
  void setSlot(uint32_t index, llvm::Value *value);
  void reserve(uint32_t n);
  void push(llvm::Value *code, llvm::Value *value);
  void putEntry(llvm::Value *index, llvm::Value *code, llvm::Value *value);
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
  std::vector<int32_t> scope_;
  std::map<uint32_t, uint32_t> lookOfMatch_;
  std::vector<CountedLoop> loops_;
  std::set<uint32_t> targets_;
  bool hasBackref_ = false, hasLook_ = false, hasLoops_ = false;
  bool anchored_ = false;
  bool memo_ = false;
  uint32_t rows_ = 0;
  std::map<uint32_t, uint32_t> blockRow_, repeatRow_;
  int64_t skipRepeat_ = -1;
  std::vector<ScanOffset> scan_;
  bool scanUnits_ = false;
  bool scanEmpty_ = false;
  llvm::Function *fn_ = nullptr;
  llvm::Value *units_ = nullptr, *length_ = nullptr, *start_ = nullptr, *caps_ = nullptr,
              *sticky_ = nullptr, *max_ = nullptr;
  llvm::AllocaInst *pos_ = nullptr, *top_ = nullptr, *base_ = nullptr, *capacity_ = nullptr,
                   *value_ = nullptr, *attempt_ = nullptr, *next_ = nullptr, *skip_ = nullptr,
                   *slots_ = nullptr, *result_ = nullptr, *steps_ = nullptr, *search_ = nullptr,
                   *memoAt_ = nullptr, *depth_ = nullptr, *scanAt_ = nullptr, *found_ = nullptr;
  std::map<uint32_t, std::array<llvm::AllocaInst *, 3>> lookVars_;
  llvm::BasicBlock *exit_ = nullptr;
  llvm::GlobalVariable *stackVar_ = nullptr, *capacityVar_ = nullptr;
  bool threadLocal_;
  llvm::Value *here(llvm::GlobalVariable *var);
  std::map<ByteSet, llvm::Constant *> tables_;
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

enum { kMemoPages, kMemoRows, kMemoLength, kMemoLog, kMemoLogLength, kMemoLogCapacity };

uint32_t pushesOf(const uint32_t *code, uint32_t pc);

int64_t followingChar(const uint32_t *code, uint32_t pc, bool unicode);

}
