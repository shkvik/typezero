#include "compiler/lower/library/regex/matcher_repeats/matcher_repeats.h"
#include "compiler/lower/library/regex/matcher_analysis/matcher_analysis.h"
#include "compiler/lower/library/regex/matcher_chars/matcher_chars.h"
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

MatcherRepeats::MatcherRepeats(MatcherBuilder &builder)
    : builder(builder) {}

FirstUnits MatcherRepeats::unitsFirst(uint32_t pc) const {
  FirstUnits units;
  std::set<uint32_t> states = {pc};
  bool match = false, unknown = false;
  this->builder.matcherAnalysis().closure(states, match, unknown);
  if (match || unknown || states.empty()) {
    return units;
  }
  for (uint32_t state : states) {
    const uint32_t *op = this->builder.code + state;
    uint32_t atom = op[0] == RE_REPEAT ? state + 5 : state;
    const uint32_t *a = this->builder.code + atom;
    switch (a[0]) {
    case RE_CHAR:
      if (a[1] < 256) {
        MatcherBuilder::addByte(units.bytes, a[1]);
      } else {
        units.wide = true;
      }
      break;
    case RE_SET:
      for (int i = 0; i < 8; ++i) {
        units.bytes[i] |= a[2 + i];
      }
      if (a[1] > 0) {
        units.wide = true;
      }
      break;
    case RE_ANY:
    case RE_ALL:
      for (uint32_t c = 0; c < 256; ++c) {
        if (a[0] == RE_ALL || (c != '\n' && c != '\r')) {
          MatcherBuilder::addByte(units.bytes, c);
        }
      }
      units.wide = true;
      break;
    default:
      return units;
    }
  }
  units.known = true;

  return units;
}

Value *MatcherRepeats::startsWith(const FirstUnits &units, Value *pos) {
  return this->builder.matcherChars().test(
      this->builder.b.CreateICmpSLT(pos, this->builder.length), [&] {
        Value *u = this->builder.unitAt(pos);
        Value *in = this->builder.matcherChars().inByteSet(u, units.bytes);
        if (this->builder.wide && units.wide) {
          in = this->builder.b.CreateOr(in,
                                        this->builder.b.CreateICmpUGE(u, this->builder.i32(256)));
        } else if (this->builder.wide && this->builder.unicode) {
          in = this->builder.b.CreateOr(
              in,
              this->builder.b.CreateICmpULE(this->builder.b.CreateSub(u, this->builder.i32(0xD800)),
                                            this->builder.i32(0x7FF)));
        }

        return in;
      });
}

Value *MatcherRepeats::unitMatches(uint32_t atom, Value *u) {
  const uint32_t *op = this->builder.code + atom;
  switch (op[0]) {
  case RE_CHAR:
    return this->builder.b.CreateICmpEQ(u, this->builder.i32(op[1]));
  case RE_SET:
    return this->builder.matcherChars().inSet(u, op + 1, !this->builder.wide);
  case RE_ANY:
    return this->builder.b.CreateNot(this->builder.matcherChars().isLineTerminator(u));
  default:
    return this->builder.b.getTrue();
  }
}

void MatcherRepeats::emitRepeatGreedy(uint32_t pc, bool memo) {
  const uint32_t *op = this->builder.code + pc;
  uint32_t min = op[1], max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  bool back = MatcherBuilder::isBack(this->builder.code[atom]);
  llvm::Type *i64 = this->builder.b.getInt64Ty();
  Value *start = this->builder.load(this->builder.pos);
  llvm::AllocaInst *at = this->builder.entryVar("repeat.at"),
                   *count = this->builder.entryVar("repeat.count");
  llvm::AllocaInst *minAt = this->builder.entryVar("repeat.min"),
                   *last = this->builder.entryVar("repeat.last");
  this->builder.store(start, at);
  this->builder.store(this->builder.i32(0), count);
  this->builder.store(start, minAt);
  BasicBlock *done = this->builder.newBlock("repeat.done");
  if (!memo && !back && (!this->builder.wide || !this->builder.unicode)) {
    Value *limit = this->builder.length;
    if (max != RE_INFINITY) {
      Value *bound = this->builder.b.CreateAdd(this->builder.b.CreateSExt(start, i64),
                                               this->builder.b.getInt64(max));
      limit = this->builder.b.CreateTrunc(
          this->builder.b.CreateBinaryIntrinsic(
              llvm::Intrinsic::smin, bound, this->builder.b.CreateSExt(this->builder.length, i64)),
          this->builder.b.getInt32Ty());
    }
    BasicBlock *loop = this->builder.newBlock("repeat.loop"),
               *read = this->builder.newBlock("repeat.read"),
               *step = this->builder.newBlock("repeat.step");
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(loop);
    Value *p = this->builder.load(at);
    this->builder.b.CreateCondBr(this->builder.b.CreateICmpSLT(p, limit), read, done);
    this->builder.b.SetInsertPoint(read);
    this->builder.b.CreateCondBr(unitMatches(atom, this->builder.unitAt(p)), step, done);
    this->builder.b.SetInsertPoint(step);
    this->builder.store(this->builder.b.CreateAdd(p, this->builder.i32(1)), at);
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(done);
    Value *end = this->builder.load(at);
    if (int64_t(pc) == this->builder.skipRepeat) {
      this->builder.store(end, this->builder.skip);
    }
    this->builder.failUnless(this->builder.b.CreateICmpUGE(this->builder.b.CreateSub(end, start),
                                                           this->builder.i32(min)));
    this->builder.store(this->builder.b.CreateAdd(start, this->builder.i32(min)), minAt);
  } else if (!memo) {
    BasicBlock *loop = this->builder.newBlock("repeat.loop"),
               *read = this->builder.newBlock("repeat.read");
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(loop);
    Value *n = this->builder.load(count);
    if (max != RE_INFINITY) {
      this->builder.b.CreateCondBr(this->builder.b.CreateICmpULT(n, this->builder.i32(max)), read,
                                   done);
    } else {
      this->builder.b.CreateBr(read);
    }
    this->builder.b.SetInsertPoint(read);
    Value *np = this->builder.matcherChars().charStep(atom, this->builder.load(at), done);
    this->builder.store(np, at);
    Value *counted = this->builder.b.CreateAdd(n, this->builder.i32(1));
    this->builder.store(counted, count);
    if (min > 0) {
      BasicBlock *reached = this->builder.newBlock("repeat.min");
      this->builder.b.CreateCondBr(this->builder.b.CreateICmpEQ(counted, this->builder.i32(min)),
                                   reached, loop);
      this->builder.b.SetInsertPoint(reached);
      this->builder.store(np, minAt);
    }
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(done);
    if (int64_t(pc) == this->builder.skipRepeat) {
      this->builder.store(this->builder.load(at), this->builder.skip);
    }
    this->builder.failUnless(
        this->builder.b.CreateICmpUGE(this->builder.load(count), this->builder.i32(min)));
  } else {
    if (min > 0) {
      BasicBlock *loop = this->builder.newBlock("repeat.first"),
                 *read = this->builder.newBlock("repeat.first.read");
      BasicBlock *reached = this->builder.newBlock("repeat.first.done");
      this->builder.b.CreateBr(loop);
      this->builder.b.SetInsertPoint(loop);
      Value *n = this->builder.load(count);
      this->builder.b.CreateCondBr(this->builder.b.CreateICmpULT(n, this->builder.i32(min)), read,
                                   reached);
      this->builder.b.SetInsertPoint(read);
      this->builder.store(
          this->builder.matcherChars().charStep(atom, this->builder.load(at), this->builder.fail),
          at);
      this->builder.store(this->builder.b.CreateAdd(n, this->builder.i32(1)), count);
      this->builder.b.CreateBr(loop);
      this->builder.b.SetInsertPoint(reached);
      this->builder.store(this->builder.load(at), minAt);
    }
    this->builder.store(this->builder.i32(0xFFFFFFFFu), last);
    BasicBlock *loop = this->builder.newBlock("repeat.loop"),
               *seen = this->builder.newBlock("repeat.seen");
    BasicBlock *exhausted = this->builder.newBlock("repeat.end");
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(loop);
    Value *p = this->builder.load(at);
    this->builder.matcherMemo().memoCheck(this->builder.repeatRow[pc], pc, p, seen);
    this->builder.store(p, last);
    this->builder.store(this->builder.matcherChars().charStep(atom, p, exhausted), at);
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(seen);
    this->builder.b.CreateBr(exhausted);
    this->builder.b.SetInsertPoint(exhausted);
    Value *end = this->builder.load(last);
    this->builder.failUnless(this->builder.b.CreateICmpNE(end, this->builder.i32(0xFFFFFFFFu)));
    this->builder.store(end, at);
    if (int64_t(pc) == this->builder.skipRepeat) {
      this->builder.store(end, this->builder.skip);
    }
    this->builder.b.CreateBr(done);
    this->builder.b.SetInsertPoint(done);
  }

  this->builder.matcherMemo().countWork(start, this->builder.load(at));

  int64_t hint =
      back ? -1 : MatcherBuilder::followingChar(this->builder.code, after, this->builder.unicode);
  if (hint >= 0) {
    if (!this->builder.wide && hint > 255) {
      this->builder.b.CreateBr(this->builder.fail);
      this->builder.b.SetInsertPoint(this->builder.newBlock("never"));
    } else {
      BasicBlock *loop = this->builder.newBlock("repeat.back"),
                 *step = this->builder.newBlock("repeat.back.step"),
                 *found = this->builder.newBlock("repeat.back.done");
      Value *minPos = this->builder.load(minAt);
      this->builder.b.CreateBr(loop);
      this->builder.b.SetInsertPoint(loop);
      Value *q = this->builder.load(at);
      this->builder.failUnless(this->builder.b.CreateICmpSGE(q, minPos));
      Value *there = this->builder.matcherChars().test(
          this->builder.b.CreateICmpSLT(q, this->builder.length), [&] {
            return this->builder.b.CreateICmpEQ(this->builder.unitAt(q),
                                                this->builder.i32(uint32_t(hint)));
          });
      this->builder.b.CreateCondBr(there, found, step);
      this->builder.b.SetInsertPoint(step);
      this->builder.store(this->builder.b.CreateSub(q, this->builder.i32(1)), at);
      this->builder.b.CreateBr(loop);
      this->builder.b.SetInsertPoint(found);
    }
  }

  Value *end = this->builder.load(at), *minPos = this->builder.load(minAt);
  BasicBlock *choice = this->builder.newBlock("repeat.choice"),
             *go = this->builder.newBlock("repeat.go");
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpNE(end, minPos), choice, go);
  this->builder.b.SetInsertPoint(choice);
  this->builder.push(this->builder.i32(0), minPos);
  this->builder.push(this->builder.i32(greedyLabel(pc)), end);
  this->builder.b.CreateBr(go);
  this->builder.b.SetInsertPoint(go);
  this->builder.store(end, this->builder.pos);
  this->builder.b.CreateBr(this->builder.blockAt(after));
}

uint32_t MatcherRepeats::greedyLabel(uint32_t pc) {
  auto it = this->builder.repeatLabels.find(pc);
  if (it != this->builder.repeatLabels.end()) {
    return it->second;
  }
  const uint32_t *op = this->builder.code + pc;
  uint32_t atom = pc + 5, after = pc + 5 + op[4];
  bool back = MatcherBuilder::isBack(this->builder.code[atom]);
  llvm::IRBuilderBase::InsertPointGuard guard(this->builder.b);
  BasicBlock *block = this->builder.newBlock("repeat.less");
  uint32_t label = this->builder.newLabel(block);
  this->builder.repeatLabels[pc] = label;
  this->builder.b.SetInsertPoint(block);
  Value *p = this->builder.load(this->builder.value), *minPos = this->builder.dataValue();
  BasicBlock *drop = this->builder.newBlock("repeat.drop");
  Value *q;
  if (!back) {
    q = this->builder.b.CreateSub(p, this->builder.i32(1));
    if (this->builder.wide && this->builder.unicode) {
      Value *pair = this->builder.matcherChars().test(
          this->builder.b.CreateICmpSGE(this->builder.b.CreateSub(p, this->builder.i32(2)), minPos),
          [&] {
            return this->builder.b.CreateAnd(
                this->builder.matcherChars().isTrail(
                    this->builder.unitAt(this->builder.b.CreateSub(p, this->builder.i32(1)))),
                this->builder.matcherChars().isLead(
                    this->builder.unitAt(this->builder.b.CreateSub(p, this->builder.i32(2)))));
          });
      q = this->builder.b.CreateSelect(pair, this->builder.b.CreateSub(p, this->builder.i32(2)), q);
    }
    int64_t hint = MatcherBuilder::followingChar(this->builder.code, after, this->builder.unicode);
    if (hint >= 0) {
      if (!this->builder.wide && hint > 255) {
        this->builder.b.CreateBr(drop);
      } else {
        llvm::AllocaInst *at = this->builder.entryVar("repeat.hint");
        this->builder.store(q, at);
        BasicBlock *loop = this->builder.newBlock("hint.loop"),
                   *step = this->builder.newBlock("hint.step"),
                   *found = this->builder.newBlock("hint.done");
        this->builder.b.CreateBr(loop);
        this->builder.b.SetInsertPoint(loop);
        Value *x = this->builder.load(at);
        Value *more =
            this->builder.matcherChars().test(this->builder.b.CreateICmpSGT(x, minPos), [&] {
              return this->builder.b.CreateICmpNE(this->builder.unitAt(x),
                                                  this->builder.i32(uint32_t(hint)));
            });
        this->builder.b.CreateCondBr(more, step, found);
        this->builder.b.SetInsertPoint(step);
        this->builder.store(this->builder.b.CreateSub(x, this->builder.i32(1)), at);
        this->builder.b.CreateBr(loop);
        this->builder.b.SetInsertPoint(found);
        q = this->builder.load(at);
        this->builder.failUnless(this->builder.b.CreateICmpEQ(this->builder.unitAt(q),
                                                              this->builder.i32(uint32_t(hint))),
                                 drop);
      }
    }
  } else {
    q = this->builder.b.CreateAdd(p, this->builder.i32(1));
    if (this->builder.wide && this->builder.unicode) {
      Value *pair = this->builder.matcherChars().test(
          this->builder.b.CreateICmpSLE(this->builder.b.CreateAdd(p, this->builder.i32(2)), minPos),
          [&] {
            return this->builder.b.CreateAnd(
                this->builder.matcherChars().isLead(this->builder.unitAt(p)),
                this->builder.matcherChars().isTrail(
                    this->builder.unitAt(this->builder.b.CreateAdd(p, this->builder.i32(1)))));
          });
      q = this->builder.b.CreateSelect(pair, this->builder.b.CreateAdd(p, this->builder.i32(2)), q);
    }
  }
  if (this->builder.open()) {
    BasicBlock *keep = this->builder.newBlock("repeat.keep"),
               *last = this->builder.newBlock("repeat.last"),
               *go = this->builder.newBlock("repeat.resume");
    this->builder.b.CreateCondBr(this->builder.b.CreateICmpNE(q, minPos), keep, last);
    this->builder.b.SetInsertPoint(keep);
    this->builder.putEntry(this->builder.load(this->builder.top), this->builder.i32(label), q);
    this->builder.store(
        this->builder.b.CreateAdd(this->builder.load(this->builder.top), this->builder.i32(1)),
        this->builder.top);
    this->builder.b.CreateBr(go);
    this->builder.b.SetInsertPoint(last);
    this->builder.store(
        this->builder.b.CreateSub(this->builder.load(this->builder.top), this->builder.i32(1)),
        this->builder.top);
    this->builder.b.CreateBr(go);
    this->builder.b.SetInsertPoint(go);
    this->builder.store(q, this->builder.pos);
    this->builder.b.CreateBr(this->builder.blockAt(after));
  }
  this->builder.b.SetInsertPoint(drop);
  this->builder.store(
      this->builder.b.CreateSub(this->builder.load(this->builder.top), this->builder.i32(1)),
      this->builder.top);
  this->builder.b.CreateBr(this->builder.fail);

  return label;
}

void MatcherRepeats::emitRepeatLazy(uint32_t pc) {
  const uint32_t *op = this->builder.code + pc;
  uint32_t min = op[1], max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  llvm::AllocaInst *at = this->builder.entryVar("repeat.at"),
                   *count = this->builder.entryVar("repeat.count");
  this->builder.store(this->builder.load(this->builder.pos), at);
  this->builder.store(this->builder.i32(0), count);
  if (min > 0) {
    BasicBlock *loop = this->builder.newBlock("lazy.first"),
               *read = this->builder.newBlock("lazy.first.read");
    BasicBlock *reached = this->builder.newBlock("lazy.first.done");
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(loop);
    Value *n = this->builder.load(count);
    this->builder.b.CreateCondBr(this->builder.b.CreateICmpULT(n, this->builder.i32(min)), read,
                                 reached);
    this->builder.b.SetInsertPoint(read);
    this->builder.store(
        this->builder.matcherChars().charStep(atom, this->builder.load(at), this->builder.fail),
        at);
    this->builder.store(this->builder.b.CreateAdd(n, this->builder.i32(1)), count);
    this->builder.b.CreateBr(loop);
    this->builder.b.SetInsertPoint(reached);
  }
  Value *p = this->builder.load(at);
  if (this->builder.memo && max == RE_INFINITY) {
    this->builder.matcherMemo().memoCheck(this->builder.repeatRow[pc], pc, p, this->builder.fail);
  }
  if (max > min) {
    this->builder.push(this->builder.i32(0), this->builder.i32(min));
    this->builder.push(this->builder.i32(lazyLabel(pc)), p);
  }
  this->builder.store(p, this->builder.pos);
  this->builder.b.CreateBr(this->builder.blockAt(after));
}

uint32_t MatcherRepeats::lazyLabel(uint32_t pc) {
  auto it = this->builder.repeatLabels.find(pc);
  if (it != this->builder.repeatLabels.end()) {
    return it->second;
  }
  const uint32_t *op = this->builder.code + pc;
  uint32_t max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  llvm::IRBuilderBase::InsertPointGuard guard(this->builder.b);
  BasicBlock *block = this->builder.newBlock("lazy.more");
  uint32_t label = this->builder.newLabel(block);
  this->builder.repeatLabels[pc] = label;
  this->builder.b.SetInsertPoint(block);
  BasicBlock *drop = this->builder.newBlock("lazy.drop");
  Value *count = this->builder.dataValue();
  Value *np =
      this->builder.matcherChars().charStep(atom, this->builder.load(this->builder.value), drop);
  if (this->builder.memo && max == RE_INFINITY) {
    this->builder.matcherMemo().memoCheck(this->builder.repeatRow[pc], pc, np, drop);
  }
  Value *counted = this->builder.b.CreateAdd(count, this->builder.i32(1));
  BasicBlock *keep = this->builder.newBlock("lazy.keep"),
             *last = this->builder.newBlock("lazy.last"),
             *go = this->builder.newBlock("lazy.resume");
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpULT(counted, this->builder.i32(max)), keep,
                               last);
  this->builder.b.SetInsertPoint(keep);
  Value *top = this->builder.load(this->builder.top);
  this->builder.putEntry(this->builder.b.CreateSub(top, this->builder.i32(1)), this->builder.i32(0),
                         counted);
  this->builder.putEntry(top, this->builder.i32(label), np);
  this->builder.store(this->builder.b.CreateAdd(top, this->builder.i32(1)), this->builder.top);
  this->builder.b.CreateBr(go);
  this->builder.b.SetInsertPoint(last);
  this->builder.store(
      this->builder.b.CreateSub(this->builder.load(this->builder.top), this->builder.i32(1)),
      this->builder.top);
  this->builder.b.CreateBr(go);
  this->builder.b.SetInsertPoint(go);
  this->builder.store(np, this->builder.pos);
  this->builder.b.CreateBr(this->builder.blockAt(after));
  this->builder.b.SetInsertPoint(drop);
  this->builder.store(
      this->builder.b.CreateSub(this->builder.load(this->builder.top), this->builder.i32(1)),
      this->builder.top);
  this->builder.b.CreateBr(this->builder.fail);

  return label;
}

}
