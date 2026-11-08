#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/library/regex/matcher_analysis/matcher_analysis.h"
#include "compiler/lower/library/regex/matcher_chars/matcher_chars.h"
#include "compiler/lower/library/regex/matcher_head/matcher_head.h"
#include "compiler/lower/library/regex/matcher_instructions/matcher_instructions.h"
#include "compiler/lower/library/regex/matcher_memo/matcher_memo.h"
#include "compiler/lower/library/regex/matcher_repeats/matcher_repeats.h"
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

bool MatcherBuilder::hasByte(const ByteSet &s, uint32_t c) {
  return (s[c / 32] >> (c % 32)) & 1;
}

void MatcherBuilder::addByte(ByteSet &s, uint32_t c) {
  s[c / 32] |= 1u << (c % 32);
}

bool MatcherBuilder::isBack(uint32_t op) {
  return op == RE_CHAR_BACK || op == RE_ANY_BACK || op == RE_ALL_BACK || op == RE_SET_BACK;
}

bool MatcherBuilder::isCharInstruction(uint32_t op) {
  return op == RE_CHAR || op == RE_ANY || op == RE_ALL || op == RE_SET ||
         MatcherBuilder::isBack(op);
}

uint64_t MatcherBuilder::counterRange(const CountedLoop &loop) {
  return loop.max == RE_INFINITY ? uint64_t(loop.min) + 1 : uint64_t(loop.max) + 1;
}

uint32_t MatcherBuilder::instructionSize(const uint32_t *code, uint32_t pc) {
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
  default:
    return 1;
  }
}

double MatcherBuilder::byteFrequency(uint32_t c) {
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

std::vector<ByteTest> MatcherBuilder::byteTests(ByteSet s) {
  std::vector<ByteTest> tests;
  for (uint32_t c = 'a'; c <= 'z';) {
    if (!MatcherBuilder::hasByte(s, c) || !MatcherBuilder::hasByte(s, c - 0x20)) {
      ++c;
      continue;
    }
    uint32_t end = c;
    while (end + 1 <= 'z' && MatcherBuilder::hasByte(s, end + 1) &&
           MatcherBuilder::hasByte(s, end + 1 - 0x20)) {
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
    if (!MatcherBuilder::hasByte(s, c)) {
      ++c;
      continue;
    }
    uint32_t end = c;
    while (end + 1 < 256 && MatcherBuilder::hasByte(s, end + 1)) {
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
  if (!this->blocks[pc]) {
    this->blocks[pc] = newBlock("pc" + llvm::Twine(pc));
  }

  return this->blocks[pc];
}

llvm::AllocaInst *MatcherBuilder::entryVar(const llvm::Twine &name, llvm::Type *type) {
  BasicBlock &entry = this->fn->getEntryBlock();
  llvm::IRBuilder<> at(&entry, entry.begin());

  return at.CreateAlloca(type ? type : this->b.getInt32Ty(), nullptr, name);
}

void MatcherBuilder::failUnless(Value *ok, BasicBlock *failBlock) {
  BasicBlock *okBlock = newBlock("ok");
  this->b.CreateCondBr(ok, okBlock, failBlock ? failBlock : this->fail);
  this->b.SetInsertPoint(okBlock);
}

Value *MatcherBuilder::unitAt(Value *index) {
  Value *address = this->b.CreateInBoundsGEP(this->unitTy, this->units,
                                             this->b.CreateZExt(index, this->b.getInt64Ty()));

  return this->b.CreateZExt(this->b.CreateLoad(this->unitTy, address), this->b.getInt32Ty());
}

Value *MatcherBuilder::slotAddress(Value *index) {
  return this->b.CreateInBoundsGEP(this->b.getInt32Ty(), this->slots,
                                   this->b.CreateZExt(index, this->b.getInt64Ty()));
}

void MatcherBuilder::setSlot(Value *index, Value *slotValue) {
  Value *address = slotAddress(index);
  Value *old = this->b.CreateLoad(this->b.getInt32Ty(), address);
  BasicBlock *record = newBlock("undo"), *write = newBlock("set"), *done = newBlock("set.done");
  this->b.CreateCondBr(this->b.CreateICmpNE(old, slotValue), write, done);
  this->b.SetInsertPoint(write);
  BasicBlock *store = newBlock("set.store");
  this->b.CreateCondBr(this->b.CreateICmpNE(load(this->top), i32(0)), record, store);
  this->b.SetInsertPoint(record);
  push(this->b.CreateNot(index), old);
  this->b.CreateBr(store);
  this->b.SetInsertPoint(store);
  this->b.CreateStore(slotValue, address);
  this->b.CreateBr(done);
  this->b.SetInsertPoint(done);
}

void MatcherBuilder::reserve(uint32_t n) {
  if (n == 0) {
    return;
  }
  Value *need = this->b.CreateAdd(load(this->top), i32(n));
  BasicBlock *grow = newBlock("grow"), *go = newBlock("room");
  this->b.CreateCondBr(this->b.CreateICmpUGT(need, load(this->capacity)), grow, go);
  this->b.SetInsertPoint(grow);
  llvm::FunctionCallee growFn = module_.getOrInsertFunction(
      "tz_re_bt_grow",
      llvm::FunctionType::get(this->b.getPtrTy(), {this->b.getInt32Ty(), this->b.getInt32Ty()},
                              false));
  store(this->b.CreateCall(growFn, {load(this->top), need}), this->base);
  store(this->b.CreateLoad(this->b.getInt32Ty(), here(this->capacityVar)), this->capacity);
  this->b.CreateBr(go);
  this->b.SetInsertPoint(go);
}

void MatcherBuilder::push(Value *pushedCode, Value *pushedValue) {
  Value *topValue = load(this->top);
  putEntry(topValue, pushedCode, pushedValue);
  store(this->b.CreateAdd(topValue, i32(1)), this->top);
}

void MatcherBuilder::putEntry(Value *index, Value *entryCode, Value *entryValue) {
  llvm::Type *i64 = this->b.getInt64Ty();
  Value *entry = this->b.CreateOr(
      this->b.CreateZExt(entryCode, i64),
      this->b.CreateShl(this->b.CreateZExt(entryValue, i64), this->b.getInt64(32)));
  this->b.CreateStore(
      entry, this->b.CreateInBoundsGEP(i64, load(this->base), this->b.CreateZExt(index, i64)));
}

Value *MatcherBuilder::dataValue() {
  llvm::Type *i64 = this->b.getInt64Ty();
  Value *below = this->b.CreateSub(load(this->top), i32(1));
  Value *entry = this->b.CreateLoad(
      i64, this->b.CreateInBoundsGEP(i64, load(this->base), this->b.CreateZExt(below, i64)));

  return this->b.CreateTrunc(this->b.CreateLShr(entry, this->b.getInt64(32)), this->b.getInt32Ty());
}

uint32_t MatcherBuilder::resumeTo(BasicBlock *target) {
  auto it = this->resumeIds.find(target);
  if (it != this->resumeIds.end()) {
    return it->second;
  }
  llvm::IRBuilderBase::InsertPointGuard guard(this->b);
  BasicBlock *block = newBlock("resume");
  this->b.SetInsertPoint(block);
  store(load(this->value), this->pos);
  this->b.CreateBr(target);
  uint32_t id = newLabel(block);
  this->resumeIds[target] = id;

  return id;
}

uint32_t MatcherBuilder::newLabel(BasicBlock *block) {
  uint32_t id = this->nextId++;
  this->dispatch->addCase(this->b.getInt32(id), block);

  return id;
}

void MatcherBuilder::finish(Value *outcome) {
  store(outcome, this->result);
  this->b.CreateBr(this->exit);
}

llvm::Function *MatcherBuilder::build() {
  if (2 * this->captures + this->registers > kMaxSlots) {
    return nullptr;
  }
  matcherAnalysis().analyze();
  matcherAnalysis().planMemo();
  matcherAnalysis().planScan();

  llvm::Type *ptr = this->b.getPtrTy(), *i32Ty = this->b.getInt32Ty();
  auto *type =
      llvm::FunctionType::get(i32Ty, {ptr, ptr, i32Ty, i32Ty, i32Ty, ptr, i32Ty, i32Ty}, false);
  this->fn =
      llvm::Function::Create(type, llvm::GlobalValue::PrivateLinkage, "tz.regex.match", module_);
  this->fn->addFnAttr(llvm::Attribute::NoUnwind);
  this->fn->setUWTableKind(llvm::UWTableKind::Default);
  this->units = this->fn->getArg(1);
  Value *wideArgument = this->fn->getArg(2);
  this->length = this->fn->getArg(3);
  this->start = this->fn->getArg(4);
  this->caps = this->fn->getArg(5);
  this->sticky = this->fn->getArg(6);
  this->max = this->fn->getArg(7);

  this->stackVar = module_.getNamedGlobal("tz_re_bt");
  if (!this->stackVar) {
    this->stackVar = new llvm::GlobalVariable(
        module_, ptr, false, llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_bt");
  }
  this->capacityVar = module_.getNamedGlobal("tz_re_bt_capacity");
  if (!this->capacityVar) {
    this->capacityVar = new llvm::GlobalVariable(
        module_, i32Ty, false, llvm::GlobalValue::ExternalLinkage, nullptr, "tz_re_bt_capacity");
  }
  this->stackVar->setThreadLocal(this->threadLocal);
  this->capacityVar->setThreadLocal(this->threadLocal);

  BasicBlock *entry = newBlock("entry");
  this->b.SetInsertPoint(entry);
  auto var = [&](llvm::Type *t, const char *name) {
    return this->b.CreateAlloca(t, nullptr, name);
  };
  this->pos = var(i32Ty, "pos");
  this->top = var(i32Ty, "top");
  this->base = var(ptr, "stack");
  this->capacity = var(i32Ty, "capacity");
  this->value = var(i32Ty, "value");
  this->attempt = var(i32Ty, "attempt");
  this->next = var(i32Ty, "next");
  this->skip = var(i32Ty, "skip");
  this->result = var(i32Ty, "result");
  this->steps = var(i32Ty, "steps");
  this->search = var(i32Ty, "search");
  this->memoAt = var(ptr, "memo");
  this->depth = var(i32Ty, "depth");
  this->scanAt = var(i32Ty, "scan");
  this->found = var(i32Ty, "found");
  uint32_t slotCount = 2 * this->captures + this->registers;
  this->slots = this->b.CreateAlloca(llvm::ArrayType::get(i32Ty, slotCount), nullptr, "slots");
  for (uint32_t pc = 0; pc < this->codeSize;
       pc += MatcherBuilder::instructionSize(this->code, pc)) {
    if (this->code[pc] == RE_LOOK) {
      this->lookVars[pc] = {var(i32Ty, "look.mark"), var(i32Ty, "look.pos"),
                            var(i32Ty, "look.log")};
    }
  }

  this->b.CreateMemSet(this->slots, this->b.getInt8(0xFF), slotCount * 4, llvm::MaybeAlign(4));
  store(this->b.CreateLoad(ptr, here(this->stackVar)), this->base);
  store(this->b.CreateLoad(i32Ty, here(this->capacityVar)), this->capacity);
  store(i32(0), this->top);
  store(i32(0), this->depth);
  store(llvm::ConstantPointerNull::get(this->b.getPtrTy()), this->memoAt);
  store(i32(0), this->steps);
  store(i32(0), this->found);
  this->exit = newBlock("exit");
  BasicBlock *bytes = newBlock("bytes"), *unitsBlock = newBlock("units");
  this->b.CreateCondBr(this->b.CreateICmpNE(wideArgument, i32(0)), unitsBlock, bytes);

  this->b.SetInsertPoint(bytes);
  matcherHead().emitWidth(false);
  this->b.SetInsertPoint(unitsBlock);
  matcherHead().emitWidth(true);

  this->b.SetInsertPoint(this->exit);
  matcherMemo().withMemo("tz_re_memo_end", {});
  this->b.CreateRet(load(this->result));

  return this->fn;
}

uint32_t MatcherBuilder::pushesOf(const uint32_t *code, uint32_t pc) {
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

int64_t MatcherBuilder::followingChar(const uint32_t *code, uint32_t pc, bool unicode) {
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
    : threadLocal(threadLocal),
      module_(module),
      ctx(module.getContext()),
      b(this->ctx),
      program_(program),
      code(program + program[TZ_RE_H_CODE]),
      codeSize(program[TZ_RE_H_SOURCE] - program[TZ_RE_H_CODE]),
      flags(program[TZ_RE_H_FLAGS]),
      captures(program[TZ_RE_H_CAPTURES]),
      registers(program[TZ_RE_H_REGISTERS]),
      unicode((this->flags & (TZ_RE_UNICODE | TZ_RE_UNICODE_SETS)) != 0) {
  this->matcherAnalysisPart = std::make_unique<MatcherAnalysis>(*this);
  this->matcherCharsPart = std::make_unique<MatcherChars>(*this);
  this->matcherMemoPart = std::make_unique<MatcherMemo>(*this);
  this->matcherHeadPart = std::make_unique<MatcherHead>(*this);
  this->matcherInstructionsPart = std::make_unique<MatcherInstructions>(*this);
  this->matcherRepeatsPart = std::make_unique<MatcherRepeats>(*this);
}

llvm::BasicBlock *MatcherBuilder::newBlock(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(this->ctx, name, this->fn);
}

llvm::Value *MatcherBuilder::i32(uint32_t v) {
  return this->b.getInt32(v);
}

llvm::Value *MatcherBuilder::load(llvm::AllocaInst *var) {
  return this->b.CreateLoad(var->getAllocatedType(), var);
}

void MatcherBuilder::store(llvm::Value *v, llvm::AllocaInst *var) {
  this->b.CreateStore(v, var);
}

bool MatcherBuilder::open() {
  return this->b.GetInsertBlock() && !this->b.GetInsertBlock()->hasTerminator();
}

llvm::Value *MatcherBuilder::slotAddress(uint32_t index) { return slotAddress(i32(index)); }

llvm::Value *MatcherBuilder::loadSlot(uint32_t index) {
  return this->b.CreateLoad(this->b.getInt32Ty(), slotAddress(index));
}

void MatcherBuilder::setSlot(uint32_t index, llvm::Value *slotValue) {
  this->setSlot(this->i32(index), slotValue);
}

llvm::Value *MatcherBuilder::here(llvm::GlobalVariable *var) {
  return this->threadLocal ? static_cast<llvm::Value *>(this->b.CreateThreadLocalAddress(var))
                           : var;
}

MatcherBuilder::~MatcherBuilder() = default;
}
