#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/library/regex/matcher_analysis/matcher_analysis.h"
#include "compiler/library/regex/matcher_chars/matcher_chars.h"
#include "compiler/library/regex/matcher_head/matcher_head.h"
#include "compiler/library/regex/matcher_instructions/matcher_instructions.h"
#include "compiler/library/regex/matcher_memo/matcher_memo.h"
#include "compiler/library/regex/matcher_repeats/matcher_repeats.h"
#include "runtime/library/regex/regex/regex.h"
#include <array>
#include <map>
#include <set>

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

using llvm::BasicBlock;

using llvm::Value;

namespace {

bool hasByte(const ByteSet &s, uint32_t c) { return (s[c / 32] >> (c % 32)) & 1; }

} // namespace

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

double byteFrequency(uint32_t c) {
  static const char order[] = "etaoinsrhldcumfpgwybvkxjqz";
  static const double lower[] = {100, 75, 70, 65, 63, 60, 56, 54, 45, 35, 33, 25, 24,
                                 21,  19, 17, 17, 16, 15, 13, 9,  6,  2,  1,  1,  1};
  if (c == ' ') {
    return 150;
  }
  if (c >= 'a' && c <= 'z') {
    return lower[strchr(order, int(c)) - order];
  }
  if (c >= 'A' && c <= 'Z') {
    return lower[strchr(order, int(c | 0x20)) - order] / 10 + 1;
  }
  if (c >= '0' && c <= '9') {
    return 8;
  }
  if (c == '\n' || c == ',' || c == '.') {
    return 12;
  }
  if (c > ' ' && c < 127) {
    return 2;
  }

  return 0.5;
}

std::vector<ByteTest> byteTests(ByteSet s) {
  std::vector<ByteTest> tests;
  // Letters in both cases: a range after `| 0x20`.
  for (uint32_t c = 'a'; c <= 'z';) {
    if (!hasByte(s, c) || !hasByte(s, c - 0x20)) {
      ++c;
      continue;
    }
    uint32_t end = c;
    while (end + 1 <= 'z' && hasByte(s, end + 1) && hasByte(s, end + 1 - 0x20)) {
      ++end;
    }
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
    while (end + 1 < 256 && hasByte(s, end + 1)) {
      ++end;
    }
    if (end - c >= 2) {
      tests.push_back({c, end, false});
    } else {
      for (uint32_t d = c; d <= end; ++d) {
        tests.push_back({d, d, false});
      }
    }
    c = end + 1;
  }
  if (tests.size() > 4) {
    tests.clear();
  }

  return tests;
}

BasicBlock *MatcherBuilder::blockAt(uint32_t pc) {
  if (!blocks_[pc]) {
    blocks_[pc] = newBlock("pc" + llvm::Twine(pc));
  }

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
  if (n == 0) {
    return;
  }
  Value *need = b_.CreateAdd(load(top_), i32(n));
  BasicBlock *grow = newBlock("grow"), *go = newBlock("room");
  b_.CreateCondBr(b_.CreateICmpUGT(need, load(capacity_)), grow, go);
  b_.SetInsertPoint(grow);
  llvm::FunctionCallee growFn = module_.getOrInsertFunction(
      "tz_re_bt_grow",
      llvm::FunctionType::get(b_.getPtrTy(), {b_.getInt32Ty(), b_.getInt32Ty()}, false));
  store(b_.CreateCall(growFn, {load(top_), need}), base_);
  store(b_.CreateLoad(b_.getInt32Ty(), here(capacityVar_)), capacity_);
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
  if (it != resumeIds_.end()) {
    return it->second;
  }
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

llvm::Function *MatcherBuilder::build() {
  if (2 * captures_ + registers_ > kMaxSlots) {
    return nullptr;
  }
  matcherAnalysis().analyze();
  matcherAnalysis().planMemo();
  matcherAnalysis().planScan();

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
  if (!stackVar_) {
    stackVar_ = new llvm::GlobalVariable(module_, ptr, false, llvm::GlobalValue::ExternalLinkage,
                                         nullptr, "tz_re_bt");
  }
  capacityVar_ = module_.getNamedGlobal("tz_re_bt_capacity");
  if (!capacityVar_) {
    capacityVar_ = new llvm::GlobalVariable(
        module_, i32Ty, false, llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_bt_capacity");
  }
  stackVar_->setThreadLocal(threadLocal_);
  capacityVar_->setThreadLocal(threadLocal_);

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
  for (uint32_t pc = 0; pc < codeSize_; pc += instructionSize(code_, pc)) {
    if (code_[pc] == RE_LOOK) {
      lookVars_[pc] = {var(i32Ty, "look.mark"), var(i32Ty, "look.pos"), var(i32Ty, "look.log")};
    }
  }

  // Every slot holds a value from the start, so that undo records never read
  // undefined memory.
  b_.CreateMemSet(slots_, b_.getInt8(0xFF), slots * 4, llvm::MaybeAlign(4));
  store(b_.CreateLoad(ptr, here(stackVar_)), base_);
  store(b_.CreateLoad(i32Ty, here(capacityVar_)), capacity_);
  store(i32(0), top_);
  store(i32(0), depth_);
  store(llvm::ConstantPointerNull::get(b_.getPtrTy()), memoAt_);
  store(i32(0), steps_);
  store(i32(0), found_);
  exit_ = newBlock("exit");
  BasicBlock *bytes = newBlock("bytes"), *units = newBlock("units");
  b_.CreateCondBr(b_.CreateICmpNE(wide, i32(0)), units, bytes);

  b_.SetInsertPoint(bytes);
  matcherHead().emitWidth(false);
  b_.SetInsertPoint(units);
  matcherHead().emitWidth(true);

  b_.SetInsertPoint(exit_);
  matcherMemo().withMemo("tz_re_memo_end", {});
  b_.CreateRet(load(result_));

  return fn_;
}

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
      if (unicode && op[1] >= 0xD800 && (op[1] <= 0xDFFF || op[1] >= 0x10000)) {
        return -1;
      }
      return op[1];
    default:
      return -1;
    }
  }
}

MatcherBuilder::MatcherBuilder(llvm::Module &module, const uint32_t *program, bool threadLocal)
    : threadLocal_(threadLocal), module_(module), ctx_(module.getContext()), b_(ctx_),
      program_(program), code_(program + program[TZ_RE_H_CODE]),
      codeSize_(program[TZ_RE_H_SOURCE] - program[TZ_RE_H_CODE]), flags_(program[TZ_RE_H_FLAGS]),
      captures_(program[TZ_RE_H_CAPTURES]), registers_(program[TZ_RE_H_REGISTERS]),
      unicode_((flags_ & (TZ_RE_UNICODE | TZ_RE_UNICODE_SETS)) != 0) {
  matcherAnalysisPart_ = std::make_unique<MatcherAnalysis>(*this);
  matcherCharsPart_ = std::make_unique<MatcherChars>(*this);
  matcherMemoPart_ = std::make_unique<MatcherMemo>(*this);
  matcherHeadPart_ = std::make_unique<MatcherHead>(*this);
  matcherInstructionsPart_ = std::make_unique<MatcherInstructions>(*this);
  matcherRepeatsPart_ = std::make_unique<MatcherRepeats>(*this);
}

llvm::BasicBlock *MatcherBuilder::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(ctx_, name, fn_);
}

llvm::Value *MatcherBuilder::i32(uint32_t v) { return b_.getInt32(v); }

llvm::Value *MatcherBuilder::load(llvm::AllocaInst *var) {
  return b_.CreateLoad(var->getAllocatedType(), var);
}

void MatcherBuilder::store(llvm::Value *v, llvm::AllocaInst *var) { b_.CreateStore(v, var); }

bool MatcherBuilder::open() { return b_.GetInsertBlock() && !b_.GetInsertBlock()->hasTerminator(); }

llvm::Value *MatcherBuilder::slotAddress(uint32_t index) { return slotAddress(i32(index)); }

llvm::Value *MatcherBuilder::loadSlot(uint32_t index) {
  return b_.CreateLoad(b_.getInt32Ty(), slotAddress(index));
}

void MatcherBuilder::setSlot(uint32_t index, llvm::Value *value) { setSlot(i32(index), value); }

llvm::Value *MatcherBuilder::here(llvm::GlobalVariable *var) {
  return threadLocal_ ? static_cast<llvm::Value *>(b_.CreateThreadLocalAddress(var)) : var;
}

MatcherBuilder::~MatcherBuilder() = default;
} // namespace tz
