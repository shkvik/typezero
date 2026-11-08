#include "compiler/lower/library/regex/matcher_instructions/matcher_instructions.h"
#include "compiler/lower/library/regex/matcher_chars/matcher_chars.h"
#include "compiler/lower/library/regex/matcher_memo/matcher_memo.h"
#include "compiler/lower/library/regex/matcher_repeats/matcher_repeats.h"
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

MatcherInstructions::MatcherInstructions(MatcherBuilder &builder)
    : builder(builder) {}

bool MatcherInstructions::literalUnit(uint32_t pc, uint32_t &unit, uint32_t &fold) const {
  const uint32_t *op = this->builder.code + pc;
  uint32_t limit = this->builder.wide ? 0xFFFF : 0xFF;
  if (op[0] == RE_CHAR) {
    if (op[1] > limit ||
        (this->builder.wide && this->builder.unicode && op[1] >= 0xD800 && op[1] <= 0xDFFF)) {
      return false;
    }
    unit = op[1];
    fold = 0;
    return true;
  }
  if (op[0] != RE_SET || (this->builder.wide && op[1] != 0)) {
    return false;
  }
  uint32_t count = 0, first = 0;
  for (uint32_t c = 0; c < 256; ++c) {
    if ((op[2 + c / 32] >> (c % 32)) & 1) {
      if (count++ == 0) {
        first = c;
      }
    }
  }
  uint32_t lower = first | 0x20;
  if (count != 2 || first != (lower & ~0x20u) || lower < 'a' || lower > 'z' ||
      !((op[2 + lower / 32] >> (lower % 32)) & 1)) {
    return false;
  }
  unit = lower;
  fold = 0x20;

  return true;
}

uint32_t MatcherInstructions::literalRun(uint32_t pc) const {
  uint32_t run = 0, unit, fold;
  for (uint32_t at = pc; at < this->builder.codeSize && literalUnit(at, unit, fold);
       at += MatcherBuilder::instructionSize(this->builder.code, at)) {
    if (at != pc && this->builder.leader[at]) {
      break;
    }
    ++run;
  }

  return run;
}

uint32_t MatcherInstructions::emitLiteralRun(uint32_t pc, uint32_t run) {
  std::vector<std::pair<uint32_t, uint32_t>> units;
  for (uint32_t i = 0; i < run; ++i) {
    uint32_t unit, fold;
    literalUnit(pc, unit, fold);
    units.push_back({unit, fold});
    pc += MatcherBuilder::instructionSize(this->builder.code, pc);
  }
  llvm::Type *i64 = this->builder.b.getInt64Ty();
  Value *pos = this->builder.load(this->builder.pos);
  Value *end = this->builder.b.CreateAdd(this->builder.b.CreateSExt(pos, i64),
                                         this->builder.b.getInt64(run));
  this->builder.failUnless(
      this->builder.b.CreateICmpSLE(end, this->builder.b.CreateSExt(this->builder.length, i64)));
  uint32_t width = this->builder.wide ? 2 : 1, done = 0;
  while (done < run) {
    uint32_t n = 1;
    while (n * 2 * width <= 8 && done + n * 2 <= run) {
      n *= 2;
    }
    uint64_t expected = 0, mask = 0;
    for (uint32_t k = 0; k < n; ++k) {
      expected |= uint64_t(units[done + k].first) << (8 * width * k);
      mask |= uint64_t(units[done + k].second) << (8 * width * k);
    }
    llvm::Type *chunk = this->builder.b.getIntNTy(8 * width * n);
    Value *address = this->builder.b.CreateInBoundsGEP(
        this->builder.unitTy, this->builder.units,
        this->builder.b.CreateZExt(this->builder.b.CreateAdd(pos, this->builder.i32(done)), i64));
    Value *v = this->builder.b.CreateAlignedLoad(chunk, address, llvm::Align(1));
    if (mask) {
      v = this->builder.b.CreateOr(v, llvm::ConstantInt::get(chunk, mask));
    }
    this->builder.failUnless(
        this->builder.b.CreateICmpEQ(v, llvm::ConstantInt::get(chunk, expected)));
    done += n;
  }
  this->builder.store(this->builder.b.CreateAdd(pos, this->builder.i32(run)), this->builder.pos);

  return pc;
}

void MatcherInstructions::emitInstruction(uint32_t pc) {
  const uint32_t *op = this->builder.code + pc;
  uint32_t slotBase = 2 * this->builder.captures;
  this->builder.reserve(MatcherBuilder::pushesOf(this->builder.code, pc));
  switch (op[0]) {
  case RE_MATCH: {
    auto it = this->builder.lookOfMatch.find(pc);
    if (it != this->builder.lookOfMatch.end()) {
      emitLookSuccess(it->second);
    } else {
      this->builder.b.CreateBr(this->builder.success);
    }
    return;
  }
  case RE_FAIL:
    this->builder.b.CreateBr(this->builder.fail);
    return;
  case RE_CHAR:
  case RE_CHAR_BACK:
  case RE_ANY:
  case RE_ANY_BACK:
  case RE_ALL:
  case RE_ALL_BACK:
  case RE_SET:
  case RE_SET_BACK:
    this->builder.store(
        this->builder.matcherChars().charStep(pc, this->builder.load(this->builder.pos), nullptr),
        this->builder.pos);
    return;
  case RE_LINE_START:
    this->builder.failUnless(
        this->builder.b.CreateICmpEQ(this->builder.load(this->builder.pos), this->builder.i32(0)));
    return;
  case RE_LINE_START_M: {
    Value *pos = this->builder.load(this->builder.pos);
    Value *after = this->builder.matcherChars().test(
        this->builder.b.CreateICmpSGT(pos, this->builder.i32(0)), [&] {
          return this->builder.matcherChars().isLineTerminator(
              this->builder.unitAt(this->builder.b.CreateSub(pos, this->builder.i32(1))));
        });
    this->builder.failUnless(
        this->builder.b.CreateOr(this->builder.b.CreateICmpEQ(pos, this->builder.i32(0)), after));
    return;
  }
  case RE_LINE_END:
    this->builder.failUnless(
        this->builder.b.CreateICmpEQ(this->builder.load(this->builder.pos), this->builder.length));
    return;
  case RE_LINE_END_M: {
    Value *pos = this->builder.load(this->builder.pos);
    Value *before = this->builder.matcherChars().test(
        this->builder.b.CreateICmpSLT(pos, this->builder.length),
        [&] { return this->builder.matcherChars().isLineTerminator(this->builder.unitAt(pos)); });
    this->builder.failUnless(
        this->builder.b.CreateOr(this->builder.b.CreateICmpEQ(pos, this->builder.length), before));
    return;
  }
  case RE_WORD_BOUNDARY:
  case RE_NOT_WORD_BOUNDARY:
  case RE_WORD_BOUNDARY_I:
  case RE_NOT_WORD_BOUNDARY_I: {
    bool folded = op[0] == RE_WORD_BOUNDARY_I || op[0] == RE_NOT_WORD_BOUNDARY_I;
    bool boundary = op[0] == RE_WORD_BOUNDARY || op[0] == RE_WORD_BOUNDARY_I;
    Value *pos = this->builder.load(this->builder.pos);
    Value *a = this->builder.matcherChars().test(
        this->builder.b.CreateICmpSGT(pos, this->builder.i32(0)), [&] {
          return this->builder.matcherChars().isWordUnit(
              this->builder.unitAt(this->builder.b.CreateSub(pos, this->builder.i32(1))), folded);
        });
    Value *c = this->builder.matcherChars().test(
        this->builder.b.CreateICmpSLT(pos, this->builder.length),
        [&] { return this->builder.matcherChars().isWordUnit(this->builder.unitAt(pos), folded); });
    Value *differ = this->builder.b.CreateXor(a, c);
    this->builder.failUnless(boundary ? differ : this->builder.b.CreateNot(differ));
    return;
  }
  case RE_GOTO:
    if (op[1] <= pc && this->builder.memo) {
      this->builder.store(
          this->builder.b.CreateAdd(this->builder.load(this->builder.steps), this->builder.i32(1)),
          this->builder.steps);
    }
    this->builder.b.CreateBr(this->builder.blockAt(op[1]));
    return;
  case RE_SPLIT_NEXT:
  case RE_SPLIT_GOTO: {
    uint32_t first = op[0] == RE_SPLIT_NEXT ? pc + 2 : op[1];
    uint32_t later = op[0] == RE_SPLIT_NEXT ? op[1] : pc + 2;
    Value *pos = this->builder.load(this->builder.pos);
    FirstUnits firstUnits = this->builder.matcherRepeats().unitsFirst(first),
               laterUnits = this->builder.matcherRepeats().unitsFirst(later);
    if (firstUnits.known) {
      BasicBlock *take = this->builder.newBlock("split.first");
      this->builder.b.CreateCondBr(this->builder.matcherRepeats().startsWith(firstUnits, pos), take,
                                   this->builder.blockAt(later));
      this->builder.b.SetInsertPoint(take);
    }
    if (laterUnits.known) {
      BasicBlock *keep = this->builder.newBlock("split.keep"),
                 *go = this->builder.newBlock("split.go");
      this->builder.b.CreateCondBr(this->builder.matcherRepeats().startsWith(laterUnits, pos), keep,
                                   go);
      this->builder.b.SetInsertPoint(keep);
      this->builder.push(this->builder.i32(this->builder.resumeTo(this->builder.blockAt(later))),
                         pos);
      this->builder.b.CreateBr(go);
      this->builder.b.SetInsertPoint(go);
    } else {
      this->builder.push(this->builder.i32(this->builder.resumeTo(this->builder.blockAt(later))),
                         pos);
    }
    if (op[0] == RE_SPLIT_GOTO) {
      this->builder.b.CreateBr(this->builder.blockAt(first));
    }
    return;
  }
  case RE_SET_POSITION:
    this->builder.setSlot(slotBase + op[1], this->builder.load(this->builder.pos));
    return;
  case RE_SAVE: {
    Value *from = this->builder.loadSlot(slotBase + op[2]);
    this->builder.setSlot(2 * op[1], from);
    this->builder.setSlot(2 * op[1] + 1, this->builder.load(this->builder.pos));
    return;
  }
  case RE_SAVE_BACK: {
    Value *to = this->builder.loadSlot(slotBase + op[2]);
    this->builder.setSlot(2 * op[1], this->builder.load(this->builder.pos));
    this->builder.setSlot(2 * op[1] + 1, to);
    return;
  }
  case RE_RESET: {
    if (op[2] - op[1] == 1) {
      this->builder.setSlot(this->builder.i32(2 * op[1]), this->builder.i32(0xFFFFFFFFu));
      this->builder.setSlot(this->builder.i32(2 * op[1] + 1), this->builder.i32(0xFFFFFFFFu));
      return;
    }
    llvm::AllocaInst *k = this->builder.entryVar("reset");
    this->builder.store(this->builder.i32(2 * op[1]), k);
    BasicBlock *loop = this->builder.newBlock("reset.loop"),
               *body = this->builder.newBlock("reset.slot"),
               *done = this->builder.newBlock("reset.done");
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(loop);
    Value *slot = this->builder.load(k);
    this->builder.b.CreateCondBr(this->builder.b.CreateICmpULT(slot, this->builder.i32(2 * op[2])),
                                 body, done);
    this->builder.b.SetInsertPoint(body);
    this->builder.setSlot(slot, this->builder.i32(0xFFFFFFFFu));
    this->builder.store(this->builder.b.CreateAdd(slot, this->builder.i32(1)), k);
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(done);
    return;
  }
  case RE_SET_REGISTER:
    this->builder.setSlot(slotBase + op[1], this->builder.i32(op[2]));
    return;
  case RE_LOOP: {
    Value *n = this->builder.loadSlot(slotBase + op[1]);
    BasicBlock *body = this->builder.blockAt(op[5]), *exit = this->builder.blockAt(op[6]);
    BasicBlock *more = this->builder.newBlock("loop.more");
    this->builder.b.CreateCondBr(this->builder.b.CreateICmpULT(n, this->builder.i32(op[2])), body,
                                 more);
    this->builder.b.SetInsertPoint(more);
    if (op[3] != RE_INFINITY) {
      BasicBlock *choose = this->builder.newBlock("loop.choose");
      this->builder.b.CreateCondBr(this->builder.b.CreateICmpUGE(n, this->builder.i32(op[3])), exit,
                                   choose);
      this->builder.b.SetInsertPoint(choose);
    }
    if (op[4]) {
      this->builder.push(this->builder.i32(this->builder.resumeTo(exit)),
                         this->builder.load(this->builder.pos));
      this->builder.b.CreateBr(body);
    } else {
      this->builder.push(this->builder.i32(this->builder.resumeTo(body)),
                         this->builder.load(this->builder.pos));
      this->builder.b.CreateBr(exit);
    }
    return;
  }
  case RE_LOOP_END: {
    Value *n = this->builder.loadSlot(slotBase + op[1]);
    if (op[2] != RE_INFINITY) {
      Value *empty = this->builder.b.CreateICmpEQ(this->builder.load(this->builder.pos),
                                                  this->builder.loadSlot(slotBase + op[3]));
      this->builder.failUnless(this->builder.b.CreateNot(this->builder.b.CreateAnd(
          this->builder.b.CreateICmpUGE(n, this->builder.i32(op[2])), empty)));
    }
    Value *counted = this->builder.b.CreateSelect(
        this->builder.b.CreateICmpULT(n, this->builder.i32(RE_INFINITY - 1)),
        this->builder.b.CreateAdd(n, this->builder.i32(1)), n);
    this->builder.setSlot(slotBase + op[1], counted);
    if (this->builder.memo) {
      this->builder.store(
          this->builder.b.CreateAdd(this->builder.load(this->builder.steps), this->builder.i32(1)),
          this->builder.steps);
    }
    this->builder.b.CreateBr(this->builder.blockAt(op[4]));
    return;
  }
  case RE_REPEAT:
    emitRepeat(pc);
    return;
  case RE_BACKREF: {
    std::vector<uint32_t> words(op, op + 3 + op[2]);
    auto *data = llvm::ConstantDataArray::get(this->builder.ctx, words);
    auto *global =
        new llvm::GlobalVariable(this->builder.module_, data->getType(), true,
                                 llvm::GlobalValue::PrivateLinkage, data, "tz.regex.backref");
    llvm::Type *ptr = this->builder.b.getPtrTy(), *i32Ty = this->builder.b.getInt32Ty();
    llvm::FunctionCallee fn = this->builder.module_.getOrInsertFunction(
        "tz_re_backref",
        llvm::FunctionType::get(i32Ty, {ptr, i32Ty, i32Ty, ptr, i32Ty, ptr, i32Ty}, false));
    Value *next = this->builder.b.CreateCall(
        fn, {this->builder.units, this->builder.i32(this->builder.wide ? 1 : 0),
             this->builder.length, this->builder.slots, this->builder.load(this->builder.pos),
             global, this->builder.i32(this->builder.unicode ? 1 : 0)});
    this->builder.failUnless(this->builder.b.CreateICmpSGE(next, this->builder.i32(0)));
    this->builder.store(next, this->builder.pos);
    return;
  }
  case RE_LOOK:
    emitLook(pc);
    return;
  default:
    this->builder.b.CreateBr(this->builder.fail);
    return;
  }
}

void MatcherInstructions::emitRepeat(uint32_t pc) {
  if (!this->builder.code[pc + 3]) {
    this->builder.matcherRepeats().emitRepeatLazy(pc);
    return;
  }
  if (!this->builder.memo || this->builder.code[pc + 2] != RE_INFINITY) {
    this->builder.matcherRepeats().emitRepeatGreedy(pc, false);
    return;
  }
  BasicBlock *plain = this->builder.newBlock("repeat.plain"),
             *noted = this->builder.newBlock("repeat.memo");
  this->builder.b.CreateCondBr(
      this->builder.b.CreateIsNull(this->builder.load(this->builder.memoAt)), plain, noted);
  this->builder.b.SetInsertPoint(plain);
  this->builder.matcherRepeats().emitRepeatGreedy(pc, false);
  this->builder.b.SetInsertPoint(noted);
  this->builder.matcherRepeats().emitRepeatGreedy(pc, true);
}

void MatcherInstructions::emitLook(uint32_t pc) {
  const uint32_t *op = this->builder.code + pc;
  auto [mark, at, logMark] = this->builder.lookVars[pc];
  bool negative = (op[1] & RE_LOOK_NEGATIVE) != 0;
  Value *pos = this->builder.load(this->builder.pos);
  this->builder.store(pos, at);
  BasicBlock *failed = this->builder.newBlock("look.failed");
  uint32_t label = this->builder.newLabel(failed);
  this->builder.push(this->builder.i32(label), pos);
  this->builder.store(
      this->builder.b.CreateSub(this->builder.load(this->builder.top), this->builder.i32(1)), mark);
  if (this->builder.memo && this->builder.hasLook) {
    this->builder.store(
        this->builder.b.CreateAdd(this->builder.load(this->builder.depth), this->builder.i32(1)),
        this->builder.depth);
    this->builder.store(this->builder.matcherMemo().memoLogLength(), logMark);
  }
  {
    llvm::IRBuilderBase::InsertPointGuard guard(this->builder.b);
    this->builder.b.SetInsertPoint(failed);
    if (this->builder.memo && this->builder.hasLook) {
      this->builder.store(
          this->builder.b.CreateSub(this->builder.load(this->builder.depth), this->builder.i32(1)),
          this->builder.depth);
    }
    if (negative) {
      this->builder.store(this->builder.load(this->builder.value), this->builder.pos);
      this->builder.b.CreateBr(this->builder.blockAt(op[2]));
    } else {
      this->builder.b.CreateBr(this->builder.fail);
    }
  }
}

void MatcherInstructions::emitLookSuccess(uint32_t look) {
  const uint32_t *op = this->builder.code + look;
  auto [mark, at, logMark] = this->builder.lookVars[look];
  bool negative = (op[1] & RE_LOOK_NEGATIVE) != 0;
  llvm::Type *ptr = this->builder.b.getPtrTy(), *i32Ty = this->builder.b.getInt32Ty();
  if (this->builder.memo && this->builder.hasLook) {
    this->builder.store(
        this->builder.b.CreateSub(this->builder.load(this->builder.depth), this->builder.i32(1)),
        this->builder.depth);
    this->builder.matcherMemo().withMemo("tz_re_memo_forget", {this->builder.load(logMark)});
  }
  if (!negative) {
    llvm::FunctionCallee drop = this->builder.module_.getOrInsertFunction(
        "tz_re_bt_drop_choices", llvm::FunctionType::get(i32Ty, {ptr, i32Ty, i32Ty}, false));
    this->builder.store(this->builder.b.CreateCall(drop, {this->builder.load(this->builder.base),
                                                          this->builder.load(mark),
                                                          this->builder.load(this->builder.top)}),
                        this->builder.top);
    this->builder.store(this->builder.load(at), this->builder.pos);
    this->builder.b.CreateBr(this->builder.blockAt(op[2]));
  } else {
    llvm::FunctionCallee unwind = this->builder.module_.getOrInsertFunction(
        "tz_re_bt_unwind",
        llvm::FunctionType::get(this->builder.b.getVoidTy(), {ptr, i32Ty, i32Ty, ptr}, false));
    Value *m = this->builder.load(mark);
    this->builder.b.CreateCall(unwind,
                               {this->builder.load(this->builder.base), m,
                                this->builder.load(this->builder.top), this->builder.slots});
    this->builder.store(m, this->builder.top);
    this->builder.b.CreateBr(this->builder.fail);
  }
}

}
