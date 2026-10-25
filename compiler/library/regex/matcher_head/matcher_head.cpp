#include "compiler/library/regex/matcher_head/matcher_head.h"
#include "compiler/library/regex/matcher_chars/matcher_chars.h"
#include "compiler/library/regex/matcher_instructions/matcher_instructions.h"
#include "compiler/library/regex/matcher_memo/matcher_memo.h"
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

MatcherHead::MatcherHead(MatcherBuilder &builder) : builder_(builder) {}

void MatcherHead::emitWidth(bool wide) {
  builder_.wide_ = wide;
  builder_.unitTy_ = wide ? builder_.b_.getInt16Ty() : builder_.b_.getInt8Ty();
  builder_.blocks_.assign(builder_.codeSize_ + 1, nullptr);
  builder_.resumeIds_.clear();
  builder_.repeatLabels_.clear();
  builder_.nextId_ = 1;

  // The backtracking: pop the stack, apply undo records, resume a choice.
  builder_.fail_ = builder_.newBlock("fail");
  builder_.attemptFail_ = builder_.newBlock("attempt.fail");
  builder_.success_ = builder_.newBlock("success");
  BasicBlock *startBlock = builder_.b_.GetInsertBlock();
  {
    builder_.b_.SetInsertPoint(builder_.fail_);
    Value *top = builder_.load(builder_.top_);
    BasicBlock *pop = builder_.newBlock("pop"), *undo = builder_.newBlock("undo.apply"),
               *resume = builder_.newBlock("dispatch");
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpEQ(top, builder_.i32(0)), builder_.attemptFail_,
                             pop);
    builder_.b_.SetInsertPoint(pop);
    Value *below = builder_.b_.CreateSub(top, builder_.i32(1));
    builder_.store(below, builder_.top_);
    llvm::Type *i64 = builder_.b_.getInt64Ty();
    Value *entry = builder_.b_.CreateLoad(
        i64, builder_.b_.CreateInBoundsGEP(i64, builder_.load(builder_.base_),
                                           builder_.b_.CreateZExt(below, i64)));
    Value *code = builder_.b_.CreateTrunc(entry, builder_.b_.getInt32Ty());
    Value *value = builder_.b_.CreateTrunc(builder_.b_.CreateLShr(entry, builder_.b_.getInt64(32)),
                                           builder_.b_.getInt32Ty());
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpSLT(code, builder_.i32(0)), undo, resume);
    builder_.b_.SetInsertPoint(undo);
    builder_.b_.CreateStore(value, builder_.slotAddress(builder_.b_.CreateNot(code)));
    builder_.b_.CreateBr(builder_.fail_);
    builder_.b_.SetInsertPoint(resume);
    builder_.store(value, builder_.value_);
    if (builder_.memo_) {
      builder_.store(builder_.b_.CreateAdd(builder_.load(builder_.steps_), builder_.i32(1)),
                     builder_.steps_);
      builder_.matcherMemo().budgetCheck();
    }
    BasicBlock *never = builder_.newBlock("never");
    builder_.dispatch_ = builder_.b_.CreateSwitch(code, never);
    builder_.b_.SetInsertPoint(never);
    builder_.b_.CreateUnreachable();
  }

  builder_.b_.SetInsertPoint(startBlock);
  emitHead();

  // The code, block by block.
  for (uint32_t pc = 0; pc < builder_.codeSize_;) {
    if (builder_.leader_[pc]) {
      BasicBlock *block = builder_.blockAt(pc);
      if (builder_.open()) {
        builder_.b_.CreateBr(block);
      }
      builder_.b_.SetInsertPoint(block);
      if (builder_.memo_) {
        builder_.matcherMemo().memoCheck(builder_.blockRow_[pc], pc, builder_.load(builder_.pos_),
                                         builder_.fail_);
      }
    } else if (!builder_.open()) {
      pc += instructionSize(builder_.code_, pc); // nothing gets here
      continue;
    }
    if (uint32_t run = builder_.matcherInstructions().literalRun(pc); run >= 2) {
      pc = builder_.matcherInstructions().emitLiteralRun(pc, run);
      continue;
    }
    builder_.matcherInstructions().emitInstruction(pc);
    pc += instructionSize(builder_.code_, pc);
  }
  if (builder_.open()) {
    builder_.b_.CreateUnreachable();
  }
}

// The loop over where a match may start, and what comes after an attempt.
void MatcherHead::emitHead() {
  const uint32_t *first = builder_.program_ + builder_.program_[TZ_RE_H_FIRST];
  bool filter = (first[8] & TZ_RE_FIRST_KNOWN) != 0;
  // A search starts here, and again after each match of a batch.
  builder_.store(builder_.start_, builder_.next_);
  BasicBlock *begin = builder_.newBlock("begin");
  builder_.b_.CreateBr(begin);
  builder_.b_.SetInsertPoint(begin);
  Value *start = builder_.load(builder_.next_);
  builder_.store(start, builder_.search_);
  builder_.store(builder_.i32(0), builder_.steps_);
  if (builder_.wide_ && builder_.unicode_) {
    // With u, a start inside a surrogate pair is the start of the pair.
    Value *inside = builder_.matcherChars().test(
        builder_.b_.CreateAnd(builder_.b_.CreateICmpSGT(start, builder_.i32(0)),
                              builder_.b_.CreateICmpSLT(start, builder_.length_)),
        [&] {
          return builder_.b_.CreateAnd(builder_.matcherChars().isTrail(builder_.unitAt(start)),
                                       builder_.matcherChars().isLead(builder_.unitAt(
                                           builder_.b_.CreateSub(start, builder_.i32(1)))));
        });
    start = builder_.b_.CreateSelect(inside, builder_.b_.CreateSub(start, builder_.i32(1)), start);
  }
  builder_.store(start, builder_.next_);
  BasicBlock *head = builder_.newBlock("attempt.head");
  builder_.b_.CreateBr(head);

  builder_.b_.SetInsertPoint(head);
  Value *i = builder_.load(builder_.next_);
  BasicBlock *none = builder_.newBlock("none");
  if (builder_.anchored_) {
    builder_.failUnless(builder_.b_.CreateICmpEQ(i, builder_.i32(0)), none);
  }
  BasicBlock *attempt = builder_.newBlock("attempt");
  if (builder_.scanEmpty_ && !builder_.wide_) {
    builder_.b_.CreateBr(none);
  } else if (!builder_.anchored_ &&
             (filter || (!builder_.scan_.empty() && (!builder_.wide_ || builder_.scanUnits_)))) {
    BasicBlock *scan = builder_.newBlock("scan");
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpNE(builder_.sticky_, builder_.i32(0)), attempt,
                             scan);
    builder_.b_.SetInsertPoint(scan);
    if (!builder_.scan_.empty() && (!builder_.wide_ || builder_.scanUnits_)) {
      emitByteScan(i, attempt, builder_.next_, none);
    } else {
      // Unit by unit, past those that cannot start a match.
      BasicBlock *loop = builder_.newBlock("scan.loop"), *body = builder_.newBlock("scan.unit"),
                 *found = builder_.newBlock("scan.found");
      builder_.store(i, builder_.scanAt_);
      builder_.b_.CreateBr(loop);
      builder_.b_.SetInsertPoint(loop);
      Value *at = builder_.load(builder_.scanAt_);
      builder_.b_.CreateCondBr(builder_.b_.CreateICmpSLT(at, builder_.length_), body, none);
      builder_.b_.SetInsertPoint(body);
      Value *u = builder_.unitAt(at);
      ByteSet bits;
      for (int k = 0; k < 8; ++k) {
        bits[k] = first[k];
      }
      Value *can = builder_.matcherChars().inByteSet(u, bits);
      if (builder_.wide_ && (first[8] & TZ_RE_FIRST_WIDE)) {
        can = builder_.b_.CreateOr(can, builder_.b_.CreateICmpUGE(u, builder_.i32(256)));
      }
      BasicBlock *skip = builder_.newBlock("scan.next");
      builder_.b_.CreateCondBr(can, found, skip);
      builder_.b_.SetInsertPoint(skip);
      builder_.store(builder_.b_.CreateAdd(at, builder_.i32(1)), builder_.scanAt_);
      builder_.b_.CreateBr(loop);
      builder_.b_.SetInsertPoint(found);
      builder_.store(at, builder_.next_);
      builder_.b_.CreateBr(attempt);
    }
  } else {
    builder_.b_.CreateBr(attempt);
  }

  // An attempt: the captures undefined, the stack empty.
  builder_.b_.SetInsertPoint(attempt);
  i = builder_.load(builder_.next_);
  if (builder_.captures_ > 0) {
    builder_.b_.CreateMemSet(builder_.slots_, builder_.b_.getInt8(0xFF), 8 * builder_.captures_,
                             llvm::MaybeAlign(4));
  }
  builder_.store(builder_.i32(0), builder_.top_);
  builder_.store(i, builder_.attempt_);
  builder_.store(i, builder_.skip_);
  builder_.store(i, builder_.pos_);
  builder_.matcherMemo().budgetCheck();
  builder_.b_.CreateBr(builder_.blockAt(0));

  // No match from there: the next start, past a run the first repeat read.
  builder_.b_.SetInsertPoint(builder_.attemptFail_);
  BasicBlock *again = builder_.newBlock("attempt.next");
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpNE(builder_.sticky_, builder_.i32(0)), none,
                           again);
  builder_.b_.SetInsertPoint(again);
  Value *nextStart = builder_.b_.CreateAdd(builder_.load(builder_.attempt_), builder_.i32(1));
  bool skips = builder_.skipRepeat_ >= 0 && !(builder_.wide_ && builder_.unicode_ &&
                                              builder_.code_[builder_.skipRepeat_ + 1] == 0);
  if (skips) {
    nextStart = builder_.b_.CreateBinaryIntrinsic(
        llvm::Intrinsic::smax, nextStart,
        builder_.b_.CreateAdd(builder_.load(builder_.skip_), builder_.i32(1)));
  }
  builder_.store(nextStart, builder_.next_);
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpSGT(nextStart, builder_.length_), none, head);

  builder_.b_.SetInsertPoint(none);
  builder_.finish(builder_.load(builder_.found_));

  // A match: its slots after those of the matches before it in the batch.
  builder_.b_.SetInsertPoint(builder_.success_);
  Value *found = builder_.load(builder_.found_);
  Value *matchStart = builder_.load(builder_.attempt_), *matchEnd = builder_.load(builder_.pos_);
  Value *to = builder_.b_.CreateInBoundsGEP(
      builder_.b_.getInt32Ty(), builder_.caps_,
      builder_.b_.CreateZExt(builder_.b_.CreateMul(found, builder_.i32(2 * builder_.captures_)),
                             builder_.b_.getInt64Ty()));
  builder_.b_.CreateStore(matchStart, to);
  builder_.b_.CreateStore(matchEnd,
                          builder_.b_.CreateConstInBoundsGEP1_32(builder_.b_.getInt32Ty(), to, 1));
  if (builder_.captures_ > 1) {
    builder_.b_.CreateMemCpy(
        builder_.b_.CreateConstInBoundsGEP1_32(builder_.b_.getInt32Ty(), to, 2),
        llvm::MaybeAlign(4),
        builder_.b_.CreateConstInBoundsGEP1_32(builder_.b_.getInt32Ty(), builder_.slots_, 2),
        llvm::MaybeAlign(4), 8 * (builder_.captures_ - 1));
  }
  Value *counted = builder_.b_.CreateAdd(found, builder_.i32(1));
  builder_.store(counted, builder_.found_);
  BasicBlock *more = builder_.newBlock("batch.more"), *full = builder_.newBlock("batch.full");
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpEQ(counted, builder_.max_), full, more);
  builder_.b_.SetInsertPoint(full);
  builder_.finish(counted);
  // The next search: from the end of this match, past it if it was empty
  // (AdvanceStringIndex: a whole surrogate pair with u).
  builder_.b_.SetInsertPoint(more);
  if (builder_.memo_) {
    // What the memo noted on the way to this match did not fail. The next
    // searches start at its end or later, and only lookarounds look back
    // (what they noted on the way to a match is forgotten anyway), so of
    // that only the position of the end can come again.
    builder_.matcherMemo().withMemo("tz_re_memo_clear", {matchEnd});
  }
  Value *after = builder_.b_.CreateAdd(matchEnd, builder_.i32(1));
  if (builder_.wide_ && builder_.unicode_) {
    Value *pair =
        builder_.matcherChars().test(builder_.b_.CreateICmpSLT(after, builder_.length_), [&] {
          return builder_.b_.CreateAnd(builder_.matcherChars().isLead(builder_.unitAt(matchEnd)),
                                       builder_.matcherChars().isTrail(builder_.unitAt(after)));
        });
    after = builder_.b_.CreateSelect(pair, builder_.b_.CreateAdd(matchEnd, builder_.i32(2)), after);
  }
  Value *from =
      builder_.b_.CreateSelect(builder_.b_.CreateICmpEQ(matchStart, matchEnd), after, matchEnd);
  builder_.store(from, builder_.next_);
  BasicBlock *search = builder_.newBlock("batch.next");
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpSGT(from, builder_.length_), full, search);
  builder_.b_.SetInsertPoint(search);
  builder_.b_.CreateBr(begin);
}

// Which units of a vector are in `units`, by the compares of byteTests.
Value *MatcherHead::vectorIn(Value *v, const OffsetSet &units) {
  auto *vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
  Value *folded = nullptr, *in = nullptr;
  if (builder_.wide_ && units.wide) {
    in = builder_.b_.CreateICmpUGE(v, llvm::ConstantInt::get(vecTy, 256));
  }
  for (const ByteTest &t : byteTests(units.bytes)) {
    Value *x = v;
    if (t.fold) {
      if (!folded) {
        folded = builder_.b_.CreateOr(v, llvm::ConstantInt::get(vecTy, 0x20));
      }
      x = folded;
    }
    Value *one = t.lo == t.hi ? builder_.b_.CreateICmpEQ(x, llvm::ConstantInt::get(vecTy, t.lo))
                              : builder_.b_.CreateICmpULE(
                                    builder_.b_.CreateSub(x, llvm::ConstantInt::get(vecTy, t.lo)),
                                    llvm::ConstantInt::get(vecTy, t.hi - t.lo));
    in = in ? builder_.b_.CreateOr(in, one) : one;
  }

  return in;
}

// A bit for each unit of the 16 bytes from unit `at` on: which are in
// `units`.
Value *MatcherHead::unitMask(Value *at, const OffsetSet &units) {
  uint32_t lanes = builder_.wide_ ? 8 : 16;
  auto *vecTy = llvm::FixedVectorType::get(builder_.unitTy_, lanes);
  Value *address = builder_.b_.CreateInBoundsGEP(
      builder_.unitTy_, builder_.units_, builder_.b_.CreateZExt(at, builder_.b_.getInt64Ty()));
  Value *v = builder_.b_.CreateAlignedLoad(vecTy, address, llvm::Align(1));

  return builder_.b_.CreateZExt(
      builder_.b_.CreateBitCast(vectorIn(v, units), builder_.b_.getIntNTy(lanes)),
      builder_.b_.getInt32Ty());
}

// The scan for where a match may start: 32 bytes at a time for the units at
// the offsets of `scan_`, then 16, then unit by unit at the end. Stores the
// start found in `at` and goes to `found`, or to `none`.
void MatcherHead::emitByteScan(Value *from, BasicBlock *found, llvm::AllocaInst *at,
                               BasicBlock *none) {
  llvm::Type *i64 = builder_.b_.getInt64Ty();
  uint32_t reach = 0;
  for (const ScanOffset &s : builder_.scan_) {
    reach = std::max(reach, s.offset);
  }
  uint32_t lanes = builder_.wide_ ? 8 : 16;
  auto candidates = [&](Value *i) {
    Value *mask = nullptr;
    for (const ScanOffset &s : builder_.scan_) {
      Value *one = unitMask(builder_.b_.CreateAdd(i, builder_.i32(s.offset)), s.units);
      mask = mask ? builder_.b_.CreateAnd(mask, one) : one;
    }

    return mask;
  };
  // Whether `width` more bytes and the offsets fit before the end.
  auto fits = [&](Value *i, uint32_t width) {
    Value *end =
        builder_.b_.CreateAdd(builder_.b_.CreateSExt(i, i64), builder_.b_.getInt64(reach + width));

    return builder_.b_.CreateICmpSLE(end, builder_.b_.CreateSExt(builder_.length_, i64));
  };
  builder_.store(from, builder_.scanAt_);
  BasicBlock *wideHead = builder_.newBlock("scan.32"),
             *wideBody = builder_.newBlock("scan.32.body"),
             *wideNext = builder_.newBlock("scan.32.next"),
             *vecHead = builder_.newBlock("scan.vec"),
             *vecBody = builder_.newBlock("scan.vec.body"),
             *vecNext = builder_.newBlock("scan.vec.next"), *hit = builder_.newBlock("scan.hit"),
             *tailHead = builder_.newBlock("scan.tail"),
             *tailBody = builder_.newBlock("scan.tail.body"),
             *tailNext = builder_.newBlock("scan.tail.next");
  llvm::AllocaInst *bitsVar = builder_.entryVar("scan.bits");
  builder_.b_.CreateBr(wideHead);

  builder_.b_.SetInsertPoint(wideHead);
  Value *i = builder_.load(builder_.scanAt_);
  builder_.b_.CreateCondBr(fits(i, 2 * lanes), wideBody, vecHead);
  builder_.b_.SetInsertPoint(wideBody);
  Value *low = candidates(i), *high = candidates(builder_.b_.CreateAdd(i, builder_.i32(lanes)));
  Value *bits = builder_.b_.CreateOr(low, builder_.b_.CreateShl(high, builder_.i32(lanes)));
  builder_.store(bits, bitsVar);
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpNE(bits, builder_.i32(0)), hit, wideNext);
  builder_.b_.SetInsertPoint(wideNext);
  builder_.store(builder_.b_.CreateAdd(i, builder_.i32(2 * lanes)), builder_.scanAt_);
  builder_.b_.CreateBr(wideHead);

  builder_.b_.SetInsertPoint(vecHead);
  i = builder_.load(builder_.scanAt_);
  builder_.b_.CreateCondBr(fits(i, lanes), vecBody, tailHead);
  builder_.b_.SetInsertPoint(vecBody);
  bits = candidates(i);
  builder_.store(bits, bitsVar);
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpNE(bits, builder_.i32(0)), hit, vecNext);
  builder_.b_.SetInsertPoint(vecNext);
  builder_.store(builder_.b_.CreateAdd(i, builder_.i32(lanes)), builder_.scanAt_);
  builder_.b_.CreateBr(vecHead);

  builder_.b_.SetInsertPoint(hit);
  Value *first = builder_.b_.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, builder_.load(bitsVar),
                                                   builder_.b_.getTrue());
  builder_.store(builder_.b_.CreateAdd(builder_.load(builder_.scanAt_), first), at);
  builder_.b_.CreateBr(found);

  // The last bytes, one at a time: a match needs room for its offsets.
  builder_.b_.SetInsertPoint(tailHead);
  i = builder_.load(builder_.scanAt_);
  builder_.b_.CreateCondBr(
      builder_.b_.CreateICmpSLT(builder_.b_.CreateAdd(i, builder_.i32(reach)), builder_.length_),
      tailBody, none);
  builder_.b_.SetInsertPoint(tailBody);
  Value *in = nullptr;
  for (const ScanOffset &s : builder_.scan_) {
    Value *u = builder_.unitAt(builder_.b_.CreateAdd(i, builder_.i32(s.offset)));
    Value *one = builder_.matcherChars().inByteSet(u, s.units.bytes);
    if (builder_.wide_ && s.units.wide) {
      one = builder_.b_.CreateOr(one, builder_.b_.CreateICmpUGE(u, builder_.i32(256)));
    }
    in = in ? builder_.b_.CreateAnd(in, one) : one;
  }
  BasicBlock *tailHit = builder_.newBlock("scan.tail.hit");
  builder_.b_.CreateCondBr(in, tailHit, tailNext);
  builder_.b_.SetInsertPoint(tailHit);
  builder_.store(i, at);
  builder_.b_.CreateBr(found);
  builder_.b_.SetInsertPoint(tailNext);
  builder_.store(builder_.b_.CreateAdd(i, builder_.i32(1)), builder_.scanAt_);
  builder_.b_.CreateBr(tailHead);
}

} // namespace tz
