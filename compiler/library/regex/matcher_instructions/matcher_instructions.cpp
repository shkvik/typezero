#include "compiler/library/regex/matcher_instructions/matcher_instructions.h"
#include "compiler/library/regex/matcher_chars/matcher_chars.h"
#include "compiler/library/regex/matcher_memo/matcher_memo.h"
#include "compiler/library/regex/matcher_repeats/matcher_repeats.h"
#include "runtime/library/regex/regex/regex.h"
#include <array>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

MatcherInstructions::MatcherInstructions(MatcherBuilder &builder) : builder_(builder) {}

// A unit that the instruction at `pc` matches, and the bits that may differ
// (0x20 for a letter of either case, under i), when it is a character of one
// code unit read forward: one of a run that a few wide compares can check.
bool MatcherInstructions::literalUnit(uint32_t pc, uint32_t &unit, uint32_t &fold) const {
  const uint32_t *op = builder_.code_ + pc;
  uint32_t limit = builder_.wide_ ? 0xFFFF : 0xFF;
  if (op[0] == RE_CHAR) {
    if (op[1] > limit || (builder_.wide_ && builder_.unicode_ && op[1] >= 0xD800 && op[1] <= 0xDFFF)) {
      return false;
    }
    unit = op[1];
    fold = 0;
    return true;
  }
  if (op[0] != RE_SET || (builder_.wide_ && op[1] != 0)) {
    return false;
  }
  // Exactly a letter in both cases.
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

// How many instructions from `pc` on are characters of one unit each, with
// no block starting among them.
uint32_t MatcherInstructions::literalRun(uint32_t pc) const {
  uint32_t run = 0, unit, fold;
  for (uint32_t at = pc; at < builder_.codeSize_ && literalUnit(at, unit, fold);
       at += instructionSize(builder_.code_, at)) {
    if (at != pc && builder_.leader_[at]) {
      break;
    }
    ++run;
  }

  return run;
}

// The `run` characters from `pc` on: one check that they fit, then the
// units compared 8 bytes at a time. Returns the pc after them.
uint32_t MatcherInstructions::emitLiteralRun(uint32_t pc, uint32_t run) {
  std::vector<std::pair<uint32_t, uint32_t>> units; // unit, fold
  for (uint32_t i = 0; i < run; ++i) {
    uint32_t unit, fold;
    literalUnit(pc, unit, fold);
    units.push_back({unit, fold});
    pc += instructionSize(builder_.code_, pc);
  }
  llvm::Type *i64 = builder_.b_.getInt64Ty();
  Value *pos = builder_.load(builder_.pos_);
  Value *end = builder_.b_.CreateAdd(builder_.b_.CreateSExt(pos, i64), builder_.b_.getInt64(run));
  builder_.failUnless(builder_.b_.CreateICmpSLE(end, builder_.b_.CreateSExt(builder_.length_, i64)));
  uint32_t width = builder_.wide_ ? 2 : 1, done = 0;
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
    llvm::Type *chunk = builder_.b_.getIntNTy(8 * width * n);
    Value *address =
        builder_.b_.CreateInBoundsGEP(builder_.unitTy_, builder_.units_, builder_.b_.CreateZExt(builder_.b_.CreateAdd(pos, builder_.i32(done)), i64));
    Value *v = builder_.b_.CreateAlignedLoad(chunk, address, llvm::Align(1));
    if (mask) {
      v = builder_.b_.CreateOr(v, llvm::ConstantInt::get(chunk, mask));
    }
    builder_.failUnless(builder_.b_.CreateICmpEQ(v, llvm::ConstantInt::get(chunk, expected)));
    done += n;
  }
  builder_.store(builder_.b_.CreateAdd(pos, builder_.i32(run)), builder_.pos_);

  return pc;
}

void MatcherInstructions::emitInstruction(uint32_t pc) {
  const uint32_t *op = builder_.code_ + pc;
  uint32_t slotBase = 2 * builder_.captures_;
  builder_.reserve(pushesOf(builder_.code_, pc));
  switch (op[0]) {
  case RE_MATCH: {
    auto it = builder_.lookOfMatch_.find(pc);
    if (it != builder_.lookOfMatch_.end()) {
      emitLookSuccess(it->second);
    } else {
      builder_.b_.CreateBr(builder_.success_);
    }
    return;
  }
  case RE_FAIL:
    builder_.b_.CreateBr(builder_.fail_);
    return;
  case RE_CHAR:
  case RE_CHAR_BACK:
  case RE_ANY:
  case RE_ANY_BACK:
  case RE_ALL:
  case RE_ALL_BACK:
  case RE_SET:
  case RE_SET_BACK:
    builder_.store(builder_.matcherChars().charStep(pc, builder_.load(builder_.pos_), nullptr), builder_.pos_);
    return;
  case RE_LINE_START:
    builder_.failUnless(builder_.b_.CreateICmpEQ(builder_.load(builder_.pos_), builder_.i32(0)));
    return;
  case RE_LINE_START_M: {
    Value *pos = builder_.load(builder_.pos_);
    Value *after = builder_.matcherChars().test(builder_.b_.CreateICmpSGT(pos, builder_.i32(0)),
                        [&] { return builder_.matcherChars().isLineTerminator(builder_.unitAt(builder_.b_.CreateSub(pos, builder_.i32(1)))); });
    builder_.failUnless(builder_.b_.CreateOr(builder_.b_.CreateICmpEQ(pos, builder_.i32(0)), after));
    return;
  }
  case RE_LINE_END:
    builder_.failUnless(builder_.b_.CreateICmpEQ(builder_.load(builder_.pos_), builder_.length_));
    return;
  case RE_LINE_END_M: {
    Value *pos = builder_.load(builder_.pos_);
    Value *before =
        builder_.matcherChars().test(builder_.b_.CreateICmpSLT(pos, builder_.length_), [&] { return builder_.matcherChars().isLineTerminator(builder_.unitAt(pos)); });
    builder_.failUnless(builder_.b_.CreateOr(builder_.b_.CreateICmpEQ(pos, builder_.length_), before));
    return;
  }
  case RE_WORD_BOUNDARY:
  case RE_NOT_WORD_BOUNDARY:
  case RE_WORD_BOUNDARY_I:
  case RE_NOT_WORD_BOUNDARY_I: {
    bool folded = op[0] == RE_WORD_BOUNDARY_I || op[0] == RE_NOT_WORD_BOUNDARY_I;
    bool boundary = op[0] == RE_WORD_BOUNDARY || op[0] == RE_WORD_BOUNDARY_I;
    Value *pos = builder_.load(builder_.pos_);
    Value *a = builder_.matcherChars().test(builder_.b_.CreateICmpSGT(pos, builder_.i32(0)),
                    [&] { return builder_.matcherChars().isWordUnit(builder_.unitAt(builder_.b_.CreateSub(pos, builder_.i32(1))), folded); });
    Value *c =
        builder_.matcherChars().test(builder_.b_.CreateICmpSLT(pos, builder_.length_), [&] { return builder_.matcherChars().isWordUnit(builder_.unitAt(pos), folded); });
    Value *differ = builder_.b_.CreateXor(a, c);
    builder_.failUnless(boundary ? differ : builder_.b_.CreateNot(differ));
    return;
  }
  case RE_GOTO:
    if (op[1] <= pc && builder_.memo_) {
      builder_.store(builder_.b_.CreateAdd(builder_.load(builder_.steps_), builder_.i32(1)), builder_.steps_); // an iteration: work
    }
    builder_.b_.CreateBr(builder_.blockAt(op[1]));
    return;
  case RE_SPLIT_NEXT:
  case RE_SPLIT_GOTO: {
    // The way tried first and the one left for later, each with the
    // character it needs first if it is known: a way without its character
    // would fail at once, so it is not taken, nor is a choice point made.
    uint32_t first = op[0] == RE_SPLIT_NEXT ? pc + 2 : op[1];
    uint32_t later = op[0] == RE_SPLIT_NEXT ? op[1] : pc + 2;
    Value *pos = builder_.load(builder_.pos_);
    FirstUnits firstUnits = builder_.matcherRepeats().unitsFirst(first), laterUnits = builder_.matcherRepeats().unitsFirst(later);
    if (firstUnits.known) {
      BasicBlock *take = builder_.newBlock("split.first");
      builder_.b_.CreateCondBr(builder_.matcherRepeats().startsWith(firstUnits, pos), take, builder_.blockAt(later));
      builder_.b_.SetInsertPoint(take);
    }
    if (laterUnits.known) {
      BasicBlock *keep = builder_.newBlock("split.keep"), *go = builder_.newBlock("split.go");
      builder_.b_.CreateCondBr(builder_.matcherRepeats().startsWith(laterUnits, pos), keep, go);
      builder_.b_.SetInsertPoint(keep);
      builder_.push(builder_.i32(builder_.resumeTo(builder_.blockAt(later))), pos);
      builder_.b_.CreateBr(go);
      builder_.b_.SetInsertPoint(go);
    } else {
      builder_.push(builder_.i32(builder_.resumeTo(builder_.blockAt(later))), pos);
    }
    if (op[0] == RE_SPLIT_GOTO) {
      builder_.b_.CreateBr(builder_.blockAt(first));
    }
    return;
  }
  case RE_SET_POSITION:
    builder_.setSlot(slotBase + op[1], builder_.load(builder_.pos_));
    return;
  case RE_SAVE: {
    Value *from = builder_.loadSlot(slotBase + op[2]);
    builder_.setSlot(2 * op[1], from);
    builder_.setSlot(2 * op[1] + 1, builder_.load(builder_.pos_));
    return;
  }
  case RE_SAVE_BACK: {
    Value *to = builder_.loadSlot(slotBase + op[2]);
    builder_.setSlot(2 * op[1], builder_.load(builder_.pos_));
    builder_.setSlot(2 * op[1] + 1, to);
    return;
  }
  case RE_RESET: {
    if (op[2] - op[1] == 1) {
      builder_.setSlot(builder_.i32(2 * op[1]), builder_.i32(0xFFFFFFFFu));
      builder_.setSlot(builder_.i32(2 * op[1] + 1), builder_.i32(0xFFFFFFFFu));
      return;
    }
    // Slot by slot, in a loop.
    llvm::AllocaInst *k = builder_.entryVar("reset");
    builder_.store(builder_.i32(2 * op[1]), k);
    BasicBlock *loop = builder_.newBlock("reset.loop"), *body = builder_.newBlock("reset.slot"),
               *done = builder_.newBlock("reset.done");
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(loop);
    Value *slot = builder_.load(k);
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpULT(slot, builder_.i32(2 * op[2])), body, done);
    builder_.b_.SetInsertPoint(body);
    builder_.setSlot(slot, builder_.i32(0xFFFFFFFFu));
    builder_.store(builder_.b_.CreateAdd(slot, builder_.i32(1)), k);
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(done);
    return;
  }
  case RE_SET_REGISTER:
    builder_.setSlot(slotBase + op[1], builder_.i32(op[2]));
    return;
  case RE_LOOP: {
    // counter, min, max, greedy, body, exit
    Value *n = builder_.loadSlot(slotBase + op[1]);
    BasicBlock *body = builder_.blockAt(op[5]), *exit = builder_.blockAt(op[6]);
    BasicBlock *more = builder_.newBlock("loop.more");
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpULT(n, builder_.i32(op[2])), body, more);
    builder_.b_.SetInsertPoint(more);
    if (op[3] != RE_INFINITY) {
      BasicBlock *choose = builder_.newBlock("loop.choose");
      builder_.b_.CreateCondBr(builder_.b_.CreateICmpUGE(n, builder_.i32(op[3])), exit, choose);
      builder_.b_.SetInsertPoint(choose);
    }
    if (op[4]) {
      builder_.push(builder_.i32(builder_.resumeTo(exit)), builder_.load(builder_.pos_));
      builder_.b_.CreateBr(body);
    } else {
      builder_.push(builder_.i32(builder_.resumeTo(body)), builder_.load(builder_.pos_));
      builder_.b_.CreateBr(exit);
    }
    return;
  }
  case RE_LOOP_END: {
    // counter, min, position, loop: an optional iteration that matched
    // nothing fails.
    Value *n = builder_.loadSlot(slotBase + op[1]);
    if (op[2] != RE_INFINITY) {
      Value *empty = builder_.b_.CreateICmpEQ(builder_.load(builder_.pos_), builder_.loadSlot(slotBase + op[3]));
      builder_.failUnless(builder_.b_.CreateNot(builder_.b_.CreateAnd(builder_.b_.CreateICmpUGE(n, builder_.i32(op[2])), empty)));
    }
    Value *counted =
        builder_.b_.CreateSelect(builder_.b_.CreateICmpULT(n, builder_.i32(RE_INFINITY - 1)), builder_.b_.CreateAdd(n, builder_.i32(1)), n);
    builder_.setSlot(slotBase + op[1], counted);
    if (builder_.memo_) {
      builder_.store(builder_.b_.CreateAdd(builder_.load(builder_.steps_), builder_.i32(1)), builder_.steps_);
    }
    builder_.b_.CreateBr(builder_.blockAt(op[4]));
    return;
  }
  case RE_REPEAT:
    emitRepeat(pc);
    return;
  case RE_BACKREF: {
    std::vector<uint32_t> words(op, op + 3 + op[2]);
    auto *data = llvm::ConstantDataArray::get(builder_.ctx_, words);
    auto *global =
        new llvm::GlobalVariable(builder_.module_, data->getType(), /*isConstant=*/true,
                                 llvm::GlobalValue::PrivateLinkage, data, "tz.regex.backref");
    llvm::Type *ptr = builder_.b_.getPtrTy(), *i32Ty = builder_.b_.getInt32Ty();
    llvm::FunctionCallee fn = builder_.module_.getOrInsertFunction(
        "tz_re_backref",
        llvm::FunctionType::get(i32Ty, {ptr, i32Ty, i32Ty, ptr, i32Ty, ptr, i32Ty}, false));
    Value *next = builder_.b_.CreateCall(fn, {builder_.units_, builder_.i32(builder_.wide_ ? 1 : 0), builder_.length_, builder_.slots_, builder_.load(builder_.pos_),
                                     global, builder_.i32(builder_.unicode_ ? 1 : 0)});
    builder_.failUnless(builder_.b_.CreateICmpSGE(next, builder_.i32(0)));
    builder_.store(next, builder_.pos_);
    return;
  }
  case RE_LOOK:
    emitLook(pc);
    return;
  default:
    builder_.b_.CreateBr(builder_.fail_);
    return;
  }
}

void MatcherInstructions::emitRepeat(uint32_t pc) {
  if (!builder_.code_[pc + 3]) {
    builder_.matcherRepeats().emitRepeatLazy(pc);
    return;
  }
  if (!builder_.memo_ || builder_.code_[pc + 2] != RE_INFINITY) {
    builder_.matcherRepeats().emitRepeatGreedy(pc, false);
    return;
  }
  // Without the memo as fast as it gets; with it, a note per position.
  BasicBlock *plain = builder_.newBlock("repeat.plain"), *noted = builder_.newBlock("repeat.memo");
  builder_.b_.CreateCondBr(builder_.b_.CreateIsNull(builder_.load(builder_.memoAt_)), plain, noted);
  builder_.b_.SetInsertPoint(plain);
  builder_.matcherRepeats().emitRepeatGreedy(pc, false);
  builder_.b_.SetInsertPoint(noted);
  builder_.matcherRepeats().emitRepeatGreedy(pc, true);
}

// A lookaround (kind, next) runs its body right here, above a barrier on
// the stack whose label means "the body failed".
void MatcherInstructions::emitLook(uint32_t pc) {
  const uint32_t *op = builder_.code_ + pc;
  auto [mark, at, logMark] = builder_.lookVars_[pc];
  bool negative = (op[1] & RE_LOOK_NEGATIVE) != 0;
  Value *pos = builder_.load(builder_.pos_);
  builder_.store(pos, at);
  BasicBlock *failed = builder_.newBlock("look.failed");
  uint32_t label = builder_.newLabel(failed);
  builder_.push(builder_.i32(label), pos);
  builder_.store(builder_.b_.CreateSub(builder_.load(builder_.top_), builder_.i32(1)), mark);
  if (builder_.memo_ && builder_.hasLook_) {
    builder_.store(builder_.b_.CreateAdd(builder_.load(builder_.depth_), builder_.i32(1)), builder_.depth_);
    builder_.store(builder_.matcherMemo().memoLogLength(), logMark);
  }
  {
    llvm::IRBuilderBase::InsertPointGuard guard(builder_.b_);
    builder_.b_.SetInsertPoint(failed);
    if (builder_.memo_ && builder_.hasLook_) {
      builder_.store(builder_.b_.CreateSub(builder_.load(builder_.depth_), builder_.i32(1)), builder_.depth_);
    }
    if (negative) {
      builder_.store(builder_.load(builder_.value_), builder_.pos_);
      builder_.b_.CreateBr(builder_.blockAt(op[2]));
    } else {
      builder_.b_.CreateBr(builder_.fail_);
    }
  }
}

// The end of a lookaround's body: it matched.
void MatcherInstructions::emitLookSuccess(uint32_t look) {
  const uint32_t *op = builder_.code_ + look;
  auto [mark, at, logMark] = builder_.lookVars_[look];
  bool negative = (op[1] & RE_LOOK_NEGATIVE) != 0;
  llvm::Type *ptr = builder_.b_.getPtrTy(), *i32Ty = builder_.b_.getInt32Ty();
  if (builder_.memo_ && builder_.hasLook_) {
    builder_.store(builder_.b_.CreateSub(builder_.load(builder_.depth_), builder_.i32(1)), builder_.depth_);
    // What the memo noted on the way may have led here: forgotten.
    builder_.matcherMemo().withMemo("tz_re_memo_forget", {builder_.load(logMark)});
  }
  if (!negative) {
    llvm::FunctionCallee drop = builder_.module_.getOrInsertFunction(
        "tz_re_bt_drop_choices", llvm::FunctionType::get(i32Ty, {ptr, i32Ty, i32Ty}, false));
    builder_.store(builder_.b_.CreateCall(drop, {builder_.load(builder_.base_), builder_.load(mark), builder_.load(builder_.top_)}), builder_.top_);
    builder_.store(builder_.load(at), builder_.pos_);
    builder_.b_.CreateBr(builder_.blockAt(op[2]));
  } else {
    llvm::FunctionCallee unwind = builder_.module_.getOrInsertFunction(
        "tz_re_bt_unwind",
        llvm::FunctionType::get(builder_.b_.getVoidTy(), {ptr, i32Ty, i32Ty, ptr}, false));
    Value *m = builder_.load(mark);
    builder_.b_.CreateCall(unwind, {builder_.load(builder_.base_), m, builder_.load(builder_.top_), builder_.slots_});
    builder_.store(m, builder_.top_);
    builder_.b_.CreateBr(builder_.fail_);
  }
}

} // namespace tz
