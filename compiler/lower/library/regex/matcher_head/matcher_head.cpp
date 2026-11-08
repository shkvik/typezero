#include "compiler/lower/library/regex/matcher_head/matcher_head.h"
#include "compiler/lower/library/regex/matcher_chars/matcher_chars.h"
#include "compiler/lower/library/regex/matcher_instructions/matcher_instructions.h"
#include "compiler/lower/library/regex/matcher_memo/matcher_memo.h"
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

MatcherHead::MatcherHead(MatcherBuilder &builder)
    : builder(builder) {}

void MatcherHead::emitWidth(bool wide) {
  this->builder.wide = wide;
  this->builder.unitTy = wide ? this->builder.b.getInt16Ty() : this->builder.b.getInt8Ty();
  this->builder.blocks.assign(this->builder.codeSize + 1, nullptr);
  this->builder.resumeIds.clear();
  this->builder.repeatLabels.clear();
  this->builder.nextId = 1;

  this->builder.fail = this->builder.newBlock("fail");
  this->builder.attemptFail = this->builder.newBlock("attempt.fail");
  this->builder.success = this->builder.newBlock("success");
  BasicBlock *startBlock = this->builder.b.GetInsertBlock();
  {
    this->builder.b.SetInsertPoint(this->builder.fail);
    Value *top = this->builder.load(this->builder.top);
    BasicBlock *pop = this->builder.newBlock("pop"), *undo = this->builder.newBlock("undo.apply"),
               *resume = this->builder.newBlock("dispatch");
    this->builder.b.CreateCondBr(this->builder.b.CreateICmpEQ(top, this->builder.i32(0)),
                                 this->builder.attemptFail, pop);
    this->builder.b.SetInsertPoint(pop);
    Value *below = this->builder.b.CreateSub(top, this->builder.i32(1));
    this->builder.store(below, this->builder.top);
    llvm::Type *i64 = this->builder.b.getInt64Ty();
    Value *entry = this->builder.b.CreateLoad(
        i64, this->builder.b.CreateInBoundsGEP(i64, this->builder.load(this->builder.base),
                                               this->builder.b.CreateZExt(below, i64)));
    Value *code = this->builder.b.CreateTrunc(entry, this->builder.b.getInt32Ty());
    Value *value =
        this->builder.b.CreateTrunc(this->builder.b.CreateLShr(entry, this->builder.b.getInt64(32)),
                                    this->builder.b.getInt32Ty());
    this->builder.b.CreateCondBr(this->builder.b.CreateICmpSLT(code, this->builder.i32(0)), undo,
                                 resume);
    this->builder.b.SetInsertPoint(undo);
    this->builder.b.CreateStore(value, this->builder.slotAddress(this->builder.b.CreateNot(code)));
    this->builder.b.CreateBr(this->builder.fail);
    this->builder.b.SetInsertPoint(resume);
    this->builder.store(value, this->builder.value);
    if (this->builder.memo) {
      this->builder.store(
          this->builder.b.CreateAdd(this->builder.load(this->builder.steps), this->builder.i32(1)),
          this->builder.steps);
      this->builder.matcherMemo().budgetCheck();
    }
    BasicBlock *never = this->builder.newBlock("never");
    this->builder.dispatch = this->builder.b.CreateSwitch(code, never);
    this->builder.b.SetInsertPoint(never);
    this->builder.b.CreateUnreachable();
  }

  this->builder.b.SetInsertPoint(startBlock);
  emitHead();

  for (uint32_t pc = 0; pc < this->builder.codeSize;) {
    if (this->builder.leader[pc]) {
      BasicBlock *block = this->builder.blockAt(pc);
      if (this->builder.open()) {
        this->builder.b.CreateBr(block);
      }
      this->builder.b.SetInsertPoint(block);
      if (this->builder.memo) {
        this->builder.matcherMemo().memoCheck(this->builder.blockRow[pc], pc,
                                              this->builder.load(this->builder.pos),
                                              this->builder.fail);
      }
    } else if (!this->builder.open()) {
      pc += MatcherBuilder::instructionSize(this->builder.code, pc);
      continue;
    }
    if (uint32_t run = this->builder.matcherInstructions().literalRun(pc); run >= 2) {
      pc = this->builder.matcherInstructions().emitLiteralRun(pc, run);
      continue;
    }
    this->builder.matcherInstructions().emitInstruction(pc);
    pc += MatcherBuilder::instructionSize(this->builder.code, pc);
  }
  if (this->builder.open()) {
    this->builder.b.CreateUnreachable();
  }
}

void MatcherHead::emitHead() {
  const uint32_t *first = this->builder.program_ + this->builder.program_[TZ_RE_H_FIRST];
  bool filter = (first[8] & TZ_RE_FIRST_KNOWN) != 0;
  this->builder.store(this->builder.start, this->builder.next);
  BasicBlock *begin = this->builder.newBlock("begin");
  this->builder.b.CreateBr(begin);
  this->builder.b.SetInsertPoint(begin);
  Value *start = this->builder.load(this->builder.next);
  this->builder.store(start, this->builder.search);
  this->builder.store(this->builder.i32(0), this->builder.steps);
  if (this->builder.wide && this->builder.unicode) {
    Value *inside = this->builder.matcherChars().test(
        this->builder.b.CreateAnd(this->builder.b.CreateICmpSGT(start, this->builder.i32(0)),
                                  this->builder.b.CreateICmpSLT(start, this->builder.length)),
        [&] {
          return this->builder.b.CreateAnd(
              this->builder.matcherChars().isTrail(this->builder.unitAt(start)),
              this->builder.matcherChars().isLead(
                  this->builder.unitAt(this->builder.b.CreateSub(start, this->builder.i32(1)))));
        });
    start = this->builder.b.CreateSelect(
        inside, this->builder.b.CreateSub(start, this->builder.i32(1)), start);
  }
  this->builder.store(start, this->builder.next);
  BasicBlock *head = this->builder.newBlock("attempt.head");
  this->builder.b.CreateBr(head);

  this->builder.b.SetInsertPoint(head);
  Value *i = this->builder.load(this->builder.next);
  BasicBlock *none = this->builder.newBlock("none");
  if (this->builder.anchored) {
    this->builder.failUnless(this->builder.b.CreateICmpEQ(i, this->builder.i32(0)), none);
  }
  BasicBlock *attempt = this->builder.newBlock("attempt");
  if (this->builder.scanEmpty && !this->builder.wide) {
    this->builder.b.CreateBr(none);
  } else if (!this->builder.anchored &&
             (filter ||
              (!this->builder.scan.empty() && (!this->builder.wide || this->builder.scanUnits)))) {
    BasicBlock *scan = this->builder.newBlock("scan");
    this->builder.b.CreateCondBr(
        this->builder.b.CreateICmpNE(this->builder.sticky, this->builder.i32(0)), attempt, scan);
    this->builder.b.SetInsertPoint(scan);
    if (!this->builder.scan.empty() && (!this->builder.wide || this->builder.scanUnits)) {
      emitByteScan(i, attempt, this->builder.next, none);
    } else {
      BasicBlock *loop = this->builder.newBlock("scan.loop"),
                 *body = this->builder.newBlock("scan.unit"),
                 *found = this->builder.newBlock("scan.found");
      this->builder.store(i, this->builder.scanAt);
      this->builder.b.CreateBr(loop);
      this->builder.b.SetInsertPoint(loop);
      Value *at = this->builder.load(this->builder.scanAt);
      this->builder.b.CreateCondBr(this->builder.b.CreateICmpSLT(at, this->builder.length), body,
                                   none);
      this->builder.b.SetInsertPoint(body);
      Value *u = this->builder.unitAt(at);
      ByteSet bits;
      for (int k = 0; k < 8; ++k) {
        bits[k] = first[k];
      }
      Value *can = this->builder.matcherChars().inByteSet(u, bits);
      if (this->builder.wide && (first[8] & TZ_RE_FIRST_WIDE)) {
        can =
            this->builder.b.CreateOr(can, this->builder.b.CreateICmpUGE(u, this->builder.i32(256)));
      }
      BasicBlock *skip = this->builder.newBlock("scan.next");
      this->builder.b.CreateCondBr(can, found, skip);
      this->builder.b.SetInsertPoint(skip);
      this->builder.store(this->builder.b.CreateAdd(at, this->builder.i32(1)),
                          this->builder.scanAt);
      this->builder.b.CreateBr(loop);
      this->builder.b.SetInsertPoint(found);
      this->builder.store(at, this->builder.next);
      this->builder.b.CreateBr(attempt);
    }
  } else {
    this->builder.b.CreateBr(attempt);
  }

  this->builder.b.SetInsertPoint(attempt);
  i = this->builder.load(this->builder.next);
  if (this->builder.captures > 0) {
    this->builder.b.CreateMemSet(this->builder.slots, this->builder.b.getInt8(0xFF),
                                 8 * this->builder.captures, llvm::MaybeAlign(4));
  }
  this->builder.store(this->builder.i32(0), this->builder.top);
  this->builder.store(i, this->builder.attempt);
  this->builder.store(i, this->builder.skip);
  this->builder.store(i, this->builder.pos);
  this->builder.matcherMemo().budgetCheck();
  this->builder.b.CreateBr(this->builder.blockAt(0));

  this->builder.b.SetInsertPoint(this->builder.attemptFail);
  BasicBlock *again = this->builder.newBlock("attempt.next");
  this->builder.b.CreateCondBr(
      this->builder.b.CreateICmpNE(this->builder.sticky, this->builder.i32(0)), none, again);
  this->builder.b.SetInsertPoint(again);
  Value *nextStart =
      this->builder.b.CreateAdd(this->builder.load(this->builder.attempt), this->builder.i32(1));
  bool skips =
      this->builder.skipRepeat >= 0 && !(this->builder.wide && this->builder.unicode &&
                                         this->builder.code[this->builder.skipRepeat + 1] == 0);
  if (skips) {
    nextStart = this->builder.b.CreateBinaryIntrinsic(
        llvm::Intrinsic::smax, nextStart,
        this->builder.b.CreateAdd(this->builder.load(this->builder.skip), this->builder.i32(1)));
  }
  this->builder.store(nextStart, this->builder.next);
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpSGT(nextStart, this->builder.length), none,
                               head);

  this->builder.b.SetInsertPoint(none);
  this->builder.finish(this->builder.load(this->builder.found));

  this->builder.b.SetInsertPoint(this->builder.success);
  Value *found = this->builder.load(this->builder.found);
  Value *matchStart = this->builder.load(this->builder.attempt),
        *matchEnd = this->builder.load(this->builder.pos);
  Value *to = this->builder.b.CreateInBoundsGEP(
      this->builder.b.getInt32Ty(), this->builder.caps,
      this->builder.b.CreateZExt(
          this->builder.b.CreateMul(found, this->builder.i32(2 * this->builder.captures)),
          this->builder.b.getInt64Ty()));
  this->builder.b.CreateStore(matchStart, to);
  this->builder.b.CreateStore(
      matchEnd, this->builder.b.CreateConstInBoundsGEP1_32(this->builder.b.getInt32Ty(), to, 1));
  if (this->builder.captures > 1) {
    this->builder.b.CreateMemCpy(
        this->builder.b.CreateConstInBoundsGEP1_32(this->builder.b.getInt32Ty(), to, 2),
        llvm::MaybeAlign(4),
        this->builder.b.CreateConstInBoundsGEP1_32(this->builder.b.getInt32Ty(),
                                                   this->builder.slots, 2),
        llvm::MaybeAlign(4), 8 * (this->builder.captures - 1));
  }
  Value *counted = this->builder.b.CreateAdd(found, this->builder.i32(1));
  this->builder.store(counted, this->builder.found);
  BasicBlock *more = this->builder.newBlock("batch.more"),
             *full = this->builder.newBlock("batch.full");
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpEQ(counted, this->builder.max), full,
                               more);
  this->builder.b.SetInsertPoint(full);
  this->builder.finish(counted);
  this->builder.b.SetInsertPoint(more);
  if (this->builder.memo) {
    this->builder.matcherMemo().withMemo("tz_re_memo_clear", {matchEnd});
  }
  Value *after = this->builder.b.CreateAdd(matchEnd, this->builder.i32(1));
  if (this->builder.wide && this->builder.unicode) {
    Value *pair = this->builder.matcherChars().test(
        this->builder.b.CreateICmpSLT(after, this->builder.length), [&] {
          return this->builder.b.CreateAnd(
              this->builder.matcherChars().isLead(this->builder.unitAt(matchEnd)),
              this->builder.matcherChars().isTrail(this->builder.unitAt(after)));
        });
    after = this->builder.b.CreateSelect(
        pair, this->builder.b.CreateAdd(matchEnd, this->builder.i32(2)), after);
  }
  Value *from = this->builder.b.CreateSelect(this->builder.b.CreateICmpEQ(matchStart, matchEnd),
                                             after, matchEnd);
  this->builder.store(from, this->builder.next);
  BasicBlock *search = this->builder.newBlock("batch.next");
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpSGT(from, this->builder.length), full,
                               search);
  this->builder.b.SetInsertPoint(search);
  this->builder.b.CreateBr(begin);
}

Value *MatcherHead::vectorIn(Value *v, const OffsetSet &units) {
  auto *vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
  Value *folded = nullptr, *in = nullptr;
  if (this->builder.wide && units.wide) {
    in = this->builder.b.CreateICmpUGE(v, llvm::ConstantInt::get(vecTy, 256));
  }
  for (const ByteTest &t : MatcherBuilder::byteTests(units.bytes)) {
    Value *x = v;
    if (t.fold) {
      if (!folded) {
        folded = this->builder.b.CreateOr(v, llvm::ConstantInt::get(vecTy, 0x20));
      }
      x = folded;
    }
    Value *one = t.lo == t.hi
                     ? this->builder.b.CreateICmpEQ(x, llvm::ConstantInt::get(vecTy, t.lo))
                     : this->builder.b.CreateICmpULE(
                           this->builder.b.CreateSub(x, llvm::ConstantInt::get(vecTy, t.lo)),
                           llvm::ConstantInt::get(vecTy, t.hi - t.lo));
    in = in ? this->builder.b.CreateOr(in, one) : one;
  }

  return in;
}

Value *MatcherHead::unitMask(Value *at, const OffsetSet &units) {
  uint32_t lanes = this->builder.wide ? 8 : 16;
  auto *vecTy = llvm::FixedVectorType::get(this->builder.unitTy, lanes);
  Value *address = this->builder.b.CreateInBoundsGEP(
      this->builder.unitTy, this->builder.units,
      this->builder.b.CreateZExt(at, this->builder.b.getInt64Ty()));
  Value *v = this->builder.b.CreateAlignedLoad(vecTy, address, llvm::Align(1));

  return this->builder.b.CreateZExt(
      this->builder.b.CreateBitCast(vectorIn(v, units), this->builder.b.getIntNTy(lanes)),
      this->builder.b.getInt32Ty());
}

void MatcherHead::emitByteScan(Value *from, BasicBlock *found, llvm::AllocaInst *at,
                               BasicBlock *none) {
  llvm::Type *i64 = this->builder.b.getInt64Ty();
  uint32_t reach = 0;
  for (const ScanOffset &s : this->builder.scan) {
    reach = std::max(reach, s.offset);
  }
  uint32_t lanes = this->builder.wide ? 8 : 16;
  auto candidates = [&](Value *i) {
    Value *mask = nullptr;
    for (const ScanOffset &s : this->builder.scan) {
      Value *one = unitMask(this->builder.b.CreateAdd(i, this->builder.i32(s.offset)), s.units);
      mask = mask ? this->builder.b.CreateAnd(mask, one) : one;
    }

    return mask;
  };
  auto fits = [&](Value *i, uint32_t width) {
    Value *end = this->builder.b.CreateAdd(this->builder.b.CreateSExt(i, i64),
                                           this->builder.b.getInt64(reach + width));

    return this->builder.b.CreateICmpSLE(end,
                                         this->builder.b.CreateSExt(this->builder.length, i64));
  };
  this->builder.store(from, this->builder.scanAt);
  BasicBlock *wideHead = this->builder.newBlock("scan.32"),
             *wideBody = this->builder.newBlock("scan.32.body"),
             *wideNext = this->builder.newBlock("scan.32.next"),
             *vecHead = this->builder.newBlock("scan.vec"),
             *vecBody = this->builder.newBlock("scan.vec.body"),
             *vecNext = this->builder.newBlock("scan.vec.next"),
             *hit = this->builder.newBlock("scan.hit"),
             *tailHead = this->builder.newBlock("scan.tail"),
             *tailBody = this->builder.newBlock("scan.tail.body"),
             *tailNext = this->builder.newBlock("scan.tail.next");
  llvm::AllocaInst *bitsVar = this->builder.entryVar("scan.bits");
  this->builder.b.CreateBr(wideHead);

  this->builder.b.SetInsertPoint(wideHead);
  Value *i = this->builder.load(this->builder.scanAt);
  this->builder.b.CreateCondBr(fits(i, 2 * lanes), wideBody, vecHead);
  this->builder.b.SetInsertPoint(wideBody);
  Value *low = candidates(i),
        *high = candidates(this->builder.b.CreateAdd(i, this->builder.i32(lanes)));
  Value *bits =
      this->builder.b.CreateOr(low, this->builder.b.CreateShl(high, this->builder.i32(lanes)));
  this->builder.store(bits, bitsVar);
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpNE(bits, this->builder.i32(0)), hit,
                               wideNext);
  this->builder.b.SetInsertPoint(wideNext);
  this->builder.store(this->builder.b.CreateAdd(i, this->builder.i32(2 * lanes)),
                      this->builder.scanAt);
  this->builder.b.CreateBr(wideHead);

  this->builder.b.SetInsertPoint(vecHead);
  i = this->builder.load(this->builder.scanAt);
  this->builder.b.CreateCondBr(fits(i, lanes), vecBody, tailHead);
  this->builder.b.SetInsertPoint(vecBody);
  bits = candidates(i);
  this->builder.store(bits, bitsVar);
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpNE(bits, this->builder.i32(0)), hit,
                               vecNext);
  this->builder.b.SetInsertPoint(vecNext);
  this->builder.store(this->builder.b.CreateAdd(i, this->builder.i32(lanes)), this->builder.scanAt);
  this->builder.b.CreateBr(vecHead);

  this->builder.b.SetInsertPoint(hit);
  Value *first = this->builder.b.CreateBinaryIntrinsic(
      llvm::Intrinsic::cttz, this->builder.load(bitsVar), this->builder.b.getTrue());
  this->builder.store(this->builder.b.CreateAdd(this->builder.load(this->builder.scanAt), first),
                      at);
  this->builder.b.CreateBr(found);

  this->builder.b.SetInsertPoint(tailHead);
  i = this->builder.load(this->builder.scanAt);
  this->builder.b.CreateCondBr(
      this->builder.b.CreateICmpSLT(this->builder.b.CreateAdd(i, this->builder.i32(reach)),
                                    this->builder.length),
      tailBody, none);
  this->builder.b.SetInsertPoint(tailBody);
  Value *in = nullptr;
  for (const ScanOffset &s : this->builder.scan) {
    Value *u = this->builder.unitAt(this->builder.b.CreateAdd(i, this->builder.i32(s.offset)));
    Value *one = this->builder.matcherChars().inByteSet(u, s.units.bytes);
    if (this->builder.wide && s.units.wide) {
      one = this->builder.b.CreateOr(one, this->builder.b.CreateICmpUGE(u, this->builder.i32(256)));
    }
    in = in ? this->builder.b.CreateAnd(in, one) : one;
  }
  BasicBlock *tailHit = this->builder.newBlock("scan.tail.hit");
  this->builder.b.CreateCondBr(in, tailHit, tailNext);
  this->builder.b.SetInsertPoint(tailHit);
  this->builder.store(i, at);
  this->builder.b.CreateBr(found);
  this->builder.b.SetInsertPoint(tailNext);
  this->builder.store(this->builder.b.CreateAdd(i, this->builder.i32(1)), this->builder.scanAt);
  this->builder.b.CreateBr(tailHead);
}

}
