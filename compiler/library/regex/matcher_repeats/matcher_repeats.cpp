#include "compiler/library/regex/matcher_repeats/matcher_repeats.h"
#include "compiler/library/regex/matcher_analysis/matcher_analysis.h"
#include "compiler/library/regex/matcher_chars/matcher_chars.h"
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

namespace {

bool isBack(uint32_t op) {
  return op == RE_CHAR_BACK || op == RE_ANY_BACK || op == RE_ALL_BACK || op == RE_SET_BACK;
}

void addByte(ByteSet &s, uint32_t c) { s[c / 32] |= 1u << (c % 32); }

} // namespace

MatcherRepeats::MatcherRepeats(MatcherBuilder &builder) : builder_(builder) {}

// The units that what the code at `pc` matches can start with, when every
// way from there reads a character forward before it can match: a split
// takes a way only if the unit at the position is one of them.
FirstUnits MatcherRepeats::unitsFirst(uint32_t pc) const {
  FirstUnits units;
  std::set<uint32_t> states = {pc};
  bool match = false, unknown = false;
  builder_.matcherAnalysis().closure(states, match, unknown);
  if (match || unknown || states.empty()) {
    return units;
  }
  for (uint32_t state : states) {
    const uint32_t *op = builder_.code_ + state;
    uint32_t atom = op[0] == RE_REPEAT ? state + 5 : state;
    const uint32_t *a = builder_.code_ + atom;
    switch (a[0]) {
    case RE_CHAR:
      if (a[1] < 256) {
        addByte(units.bytes, a[1]);
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
          addByte(units.bytes, c);
        }
      }
      units.wide = true;
      break;
    default: // read backward: the unit at the position tells nothing
      return units;
    }
  }
  units.known = true;

  return units;
}

// Whether the unit at `pos` is one of `units` (or half of a surrogate pair
// with u, which may start a character of any code point).
Value *MatcherRepeats::startsWith(const FirstUnits &units, Value *pos) {
  return builder_.matcherChars().test(builder_.b_.CreateICmpSLT(pos, builder_.length_), [&] {
    Value *u = builder_.unitAt(pos);
    Value *in = builder_.matcherChars().inByteSet(u, units.bytes);
    if (builder_.wide_ && units.wide) {
      in = builder_.b_.CreateOr(in, builder_.b_.CreateICmpUGE(u, builder_.i32(256)));
    } else if (builder_.wide_ && builder_.unicode_) {
      in = builder_.b_.CreateOr(
          in, builder_.b_.CreateICmpULE(builder_.b_.CreateSub(u, builder_.i32(0xD800)),
                                        builder_.i32(0x7FF)));
    }

    return in;
  });
}

// Whether the character at `atom` matches the unit `u`, for the repeats that
// read by code units.
Value *MatcherRepeats::unitMatches(uint32_t atom, Value *u) {
  const uint32_t *op = builder_.code_ + atom;
  switch (op[0]) {
  case RE_CHAR:
    return builder_.b_.CreateICmpEQ(u, builder_.i32(op[1]));
  case RE_SET:
    return builder_.matcherChars().inSet(u, op + 1, !builder_.wide_);
  case RE_ANY:
    return builder_.b_.CreateNot(builder_.matcherChars().isLineTerminator(u));
  default: // RE_ALL
    return builder_.b_.getTrue();
  }
}

// A greedy repeat (min, max, greedy, length, the character): as many as
// match, then one less at a time, each on a choice point of its own made
// from one entry for all (the position where the repeat ends now) above one
// that keeps where `min` ended. With `memo` (and no upper bound) the read
// also stops at a position the repeat got to before: everything from there
// on failed then.
void MatcherRepeats::emitRepeatGreedy(uint32_t pc, bool memo) {
  const uint32_t *op = builder_.code_ + pc;
  uint32_t min = op[1], max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  bool back = isBack(builder_.code_[atom]);
  llvm::Type *i64 = builder_.b_.getInt64Ty();
  Value *start = builder_.load(builder_.pos_);
  llvm::AllocaInst *at = builder_.entryVar("repeat.at"), *count = builder_.entryVar("repeat.count");
  llvm::AllocaInst *minAt = builder_.entryVar("repeat.min"),
                   *last = builder_.entryVar("repeat.last");
  builder_.store(start, at);
  builder_.store(builder_.i32(0), count);
  builder_.store(start, minAt);
  BasicBlock *done = builder_.newBlock("repeat.done");
  if (!memo && !back && (!builder_.wide_ || !builder_.unicode_)) {
    // By code units, up to the limit.
    Value *limit = builder_.length_;
    if (max != RE_INFINITY) {
      Value *bound =
          builder_.b_.CreateAdd(builder_.b_.CreateSExt(start, i64), builder_.b_.getInt64(max));
      limit = builder_.b_.CreateTrunc(
          builder_.b_.CreateBinaryIntrinsic(llvm::Intrinsic::smin, bound,
                                            builder_.b_.CreateSExt(builder_.length_, i64)),
          builder_.b_.getInt32Ty());
    }
    BasicBlock *loop = builder_.newBlock("repeat.loop"), *read = builder_.newBlock("repeat.read"),
               *step = builder_.newBlock("repeat.step");
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(loop);
    Value *p = builder_.load(at);
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpSLT(p, limit), read, done);
    builder_.b_.SetInsertPoint(read);
    builder_.b_.CreateCondBr(unitMatches(atom, builder_.unitAt(p)), step, done);
    builder_.b_.SetInsertPoint(step);
    builder_.store(builder_.b_.CreateAdd(p, builder_.i32(1)), at);
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(done);
    Value *end = builder_.load(at);
    if (int64_t(pc) == builder_.skipRepeat_) {
      builder_.store(end, builder_.skip_);
    }
    builder_.failUnless(
        builder_.b_.CreateICmpUGE(builder_.b_.CreateSub(end, start), builder_.i32(min)));
    builder_.store(builder_.b_.CreateAdd(start, builder_.i32(min)), minAt);
  } else if (!memo) {
    // Character by character.
    BasicBlock *loop = builder_.newBlock("repeat.loop"), *read = builder_.newBlock("repeat.read");
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(loop);
    Value *n = builder_.load(count);
    if (max != RE_INFINITY) {
      builder_.b_.CreateCondBr(builder_.b_.CreateICmpULT(n, builder_.i32(max)), read, done);
    } else {
      builder_.b_.CreateBr(read);
    }
    builder_.b_.SetInsertPoint(read);
    Value *np = builder_.matcherChars().charStep(atom, builder_.load(at), done);
    builder_.store(np, at);
    Value *counted = builder_.b_.CreateAdd(n, builder_.i32(1));
    builder_.store(counted, count);
    if (min > 0) {
      BasicBlock *reached = builder_.newBlock("repeat.min");
      builder_.b_.CreateCondBr(builder_.b_.CreateICmpEQ(counted, builder_.i32(min)), reached, loop);
      builder_.b_.SetInsertPoint(reached);
      builder_.store(np, minAt);
    }
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(done);
    if (int64_t(pc) == builder_.skipRepeat_) {
      builder_.store(builder_.load(at), builder_.skip_);
    }
    builder_.failUnless(builder_.b_.CreateICmpUGE(builder_.load(count), builder_.i32(min)));
  } else {
    // With the memo: `min` characters, then a note at each position.
    if (min > 0) {
      BasicBlock *loop = builder_.newBlock("repeat.first"),
                 *read = builder_.newBlock("repeat.first.read");
      BasicBlock *reached = builder_.newBlock("repeat.first.done");
      builder_.b_.CreateBr(loop);
      builder_.b_.SetInsertPoint(loop);
      Value *n = builder_.load(count);
      builder_.b_.CreateCondBr(builder_.b_.CreateICmpULT(n, builder_.i32(min)), read, reached);
      builder_.b_.SetInsertPoint(read);
      builder_.store(builder_.matcherChars().charStep(atom, builder_.load(at), builder_.fail_), at);
      builder_.store(builder_.b_.CreateAdd(n, builder_.i32(1)), count);
      builder_.b_.CreateBr(loop);
      builder_.b_.SetInsertPoint(reached);
      builder_.store(builder_.load(at), minAt);
    }
    builder_.store(builder_.i32(0xFFFFFFFFu), last);
    BasicBlock *loop = builder_.newBlock("repeat.loop"), *seen = builder_.newBlock("repeat.seen");
    BasicBlock *exhausted = builder_.newBlock("repeat.end");
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(loop);
    Value *p = builder_.load(at);
    builder_.matcherMemo().memoCheck(builder_.repeatRow_[pc], pc, p, seen);
    builder_.store(p, last);
    builder_.store(builder_.matcherChars().charStep(atom, p, exhausted), at);
    builder_.b_.CreateBr(loop);
    // The read ends at the last position noted now; none: nothing to try.
    builder_.b_.SetInsertPoint(seen);
    builder_.b_.CreateBr(exhausted);
    builder_.b_.SetInsertPoint(exhausted);
    Value *end = builder_.load(last);
    builder_.failUnless(builder_.b_.CreateICmpNE(end, builder_.i32(0xFFFFFFFFu)));
    builder_.store(end, at);
    if (int64_t(pc) == builder_.skipRepeat_) {
      builder_.store(end, builder_.skip_);
    }
    builder_.b_.CreateBr(done);
    builder_.b_.SetInsertPoint(done);
  }

  builder_.matcherMemo().countWork(start, builder_.load(at));

  // Where what follows must start with a character, the repeat ends at the
  // last position before one at once: what follows would fail at the others
  // before it did anything (as the interpreter does on backtracking).
  int64_t hint = back ? -1 : followingChar(builder_.code_, after, builder_.unicode_);
  if (hint >= 0) {
    if (!builder_.wide_ && hint > 255) {
      builder_.b_.CreateBr(builder_.fail_);
      builder_.b_.SetInsertPoint(builder_.newBlock("never"));
    } else {
      BasicBlock *loop = builder_.newBlock("repeat.back"),
                 *step = builder_.newBlock("repeat.back.step"),
                 *found = builder_.newBlock("repeat.back.done");
      Value *minPos = builder_.load(minAt);
      builder_.b_.CreateBr(loop);
      builder_.b_.SetInsertPoint(loop);
      Value *q = builder_.load(at);
      builder_.failUnless(builder_.b_.CreateICmpSGE(q, minPos));
      Value *there =
          builder_.matcherChars().test(builder_.b_.CreateICmpSLT(q, builder_.length_), [&] {
            return builder_.b_.CreateICmpEQ(builder_.unitAt(q), builder_.i32(uint32_t(hint)));
          });
      builder_.b_.CreateCondBr(there, found, step);
      builder_.b_.SetInsertPoint(step);
      builder_.store(builder_.b_.CreateSub(q, builder_.i32(1)), at);
      builder_.b_.CreateBr(loop);
      builder_.b_.SetInsertPoint(found);
    }
  }

  // The choice point for fewer characters.
  Value *end = builder_.load(at), *minPos = builder_.load(minAt);
  BasicBlock *choice = builder_.newBlock("repeat.choice"), *go = builder_.newBlock("repeat.go");
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpNE(end, minPos), choice, go);
  builder_.b_.SetInsertPoint(choice);
  builder_.push(builder_.i32(0), minPos);
  builder_.push(builder_.i32(greedyLabel(pc)), end);
  builder_.b_.CreateBr(go);
  builder_.b_.SetInsertPoint(go);
  builder_.store(end, builder_.pos_);
  builder_.b_.CreateBr(builder_.blockAt(after));
}

// The label of a greedy repeat's choice point, whose block takes one
// character less (the same with the memo and without: what follows notes
// its positions).
uint32_t MatcherRepeats::greedyLabel(uint32_t pc) {
  auto it = builder_.repeatLabels_.find(pc);
  if (it != builder_.repeatLabels_.end()) {
    return it->second;
  }
  const uint32_t *op = builder_.code_ + pc;
  uint32_t atom = pc + 5, after = pc + 5 + op[4];
  bool back = isBack(builder_.code_[atom]);
  llvm::IRBuilderBase::InsertPointGuard guard(builder_.b_);
  BasicBlock *block = builder_.newBlock("repeat.less");
  uint32_t label = builder_.newLabel(block);
  builder_.repeatLabels_[pc] = label;
  builder_.b_.SetInsertPoint(block);
  Value *p = builder_.load(builder_.value_), *minPos = builder_.dataValue();
  BasicBlock *drop = builder_.newBlock("repeat.drop");
  Value *q;
  if (!back) {
    q = builder_.b_.CreateSub(p, builder_.i32(1));
    if (builder_.wide_ && builder_.unicode_) {
      // Back over a whole pair.
      Value *pair = builder_.matcherChars().test(
          builder_.b_.CreateICmpSGE(builder_.b_.CreateSub(p, builder_.i32(2)), minPos), [&] {
            return builder_.b_.CreateAnd(builder_.matcherChars().isTrail(builder_.unitAt(
                                             builder_.b_.CreateSub(p, builder_.i32(1)))),
                                         builder_.matcherChars().isLead(builder_.unitAt(
                                             builder_.b_.CreateSub(p, builder_.i32(2)))));
          });
      q = builder_.b_.CreateSelect(pair, builder_.b_.CreateSub(p, builder_.i32(2)), q);
    }
    int64_t hint = followingChar(builder_.code_, after, builder_.unicode_);
    if (hint >= 0) {
      // Where the character that must follow is not, what follows would
      // fail: those positions are skipped.
      if (!builder_.wide_ && hint > 255) {
        builder_.b_.CreateBr(drop);
      } else {
        llvm::AllocaInst *at = builder_.entryVar("repeat.hint");
        builder_.store(q, at);
        BasicBlock *loop = builder_.newBlock("hint.loop"), *step = builder_.newBlock("hint.step"),
                   *found = builder_.newBlock("hint.done");
        builder_.b_.CreateBr(loop);
        builder_.b_.SetInsertPoint(loop);
        Value *x = builder_.load(at);
        Value *more = builder_.matcherChars().test(builder_.b_.CreateICmpSGT(x, minPos), [&] {
          return builder_.b_.CreateICmpNE(builder_.unitAt(x), builder_.i32(uint32_t(hint)));
        });
        builder_.b_.CreateCondBr(more, step, found);
        builder_.b_.SetInsertPoint(step);
        builder_.store(builder_.b_.CreateSub(x, builder_.i32(1)), at);
        builder_.b_.CreateBr(loop);
        builder_.b_.SetInsertPoint(found);
        q = builder_.load(at);
        builder_.failUnless(
            builder_.b_.CreateICmpEQ(builder_.unitAt(q), builder_.i32(uint32_t(hint))), drop);
      }
    }
  } else {
    q = builder_.b_.CreateAdd(p, builder_.i32(1));
    if (builder_.wide_ && builder_.unicode_) {
      Value *pair = builder_.matcherChars().test(
          builder_.b_.CreateICmpSLE(builder_.b_.CreateAdd(p, builder_.i32(2)), minPos), [&] {
            return builder_.b_.CreateAnd(builder_.matcherChars().isLead(builder_.unitAt(p)),
                                         builder_.matcherChars().isTrail(builder_.unitAt(
                                             builder_.b_.CreateAdd(p, builder_.i32(1)))));
          });
      q = builder_.b_.CreateSelect(pair, builder_.b_.CreateAdd(p, builder_.i32(2)), q);
    }
  }
  if (builder_.open()) {
    // Fewer still next time, unless this is `min`.
    BasicBlock *keep = builder_.newBlock("repeat.keep"), *last = builder_.newBlock("repeat.last"),
               *go = builder_.newBlock("repeat.resume");
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpNE(q, minPos), keep, last);
    builder_.b_.SetInsertPoint(keep);
    builder_.putEntry(builder_.load(builder_.top_), builder_.i32(label), q);
    builder_.store(builder_.b_.CreateAdd(builder_.load(builder_.top_), builder_.i32(1)),
                   builder_.top_);
    builder_.b_.CreateBr(go);
    builder_.b_.SetInsertPoint(last);
    builder_.store(builder_.b_.CreateSub(builder_.load(builder_.top_), builder_.i32(1)),
                   builder_.top_);
    builder_.b_.CreateBr(go);
    builder_.b_.SetInsertPoint(go);
    builder_.store(q, builder_.pos_);
    builder_.b_.CreateBr(builder_.blockAt(after));
  }
  builder_.b_.SetInsertPoint(drop);
  builder_.store(builder_.b_.CreateSub(builder_.load(builder_.top_), builder_.i32(1)),
                 builder_.top_);
  builder_.b_.CreateBr(builder_.fail_);

  return label;
}

// A lazy repeat: `min` characters, then one more at a time, each on the
// choice point, which keeps the count in the entry below it. With the memo
// on (and no upper bound) a position the repeat got to before ends it.
void MatcherRepeats::emitRepeatLazy(uint32_t pc) {
  const uint32_t *op = builder_.code_ + pc;
  uint32_t min = op[1], max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  llvm::AllocaInst *at = builder_.entryVar("repeat.at"), *count = builder_.entryVar("repeat.count");
  builder_.store(builder_.load(builder_.pos_), at);
  builder_.store(builder_.i32(0), count);
  if (min > 0) {
    BasicBlock *loop = builder_.newBlock("lazy.first"),
               *read = builder_.newBlock("lazy.first.read");
    BasicBlock *reached = builder_.newBlock("lazy.first.done");
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(loop);
    Value *n = builder_.load(count);
    builder_.b_.CreateCondBr(builder_.b_.CreateICmpULT(n, builder_.i32(min)), read, reached);
    builder_.b_.SetInsertPoint(read);
    builder_.store(builder_.matcherChars().charStep(atom, builder_.load(at), builder_.fail_), at);
    builder_.store(builder_.b_.CreateAdd(n, builder_.i32(1)), count);
    builder_.b_.CreateBr(loop);
    builder_.b_.SetInsertPoint(reached);
  }
  Value *p = builder_.load(at);
  if (builder_.memo_ && max == RE_INFINITY) {
    builder_.matcherMemo().memoCheck(builder_.repeatRow_[pc], pc, p, builder_.fail_);
  }
  if (max > min) {
    builder_.push(builder_.i32(0), builder_.i32(min));
    builder_.push(builder_.i32(lazyLabel(pc)), p);
  }
  builder_.store(p, builder_.pos_);
  builder_.b_.CreateBr(builder_.blockAt(after));
}

uint32_t MatcherRepeats::lazyLabel(uint32_t pc) {
  auto it = builder_.repeatLabels_.find(pc);
  if (it != builder_.repeatLabels_.end()) {
    return it->second;
  }
  const uint32_t *op = builder_.code_ + pc;
  uint32_t max = op[2], atom = pc + 5, after = pc + 5 + op[4];
  llvm::IRBuilderBase::InsertPointGuard guard(builder_.b_);
  BasicBlock *block = builder_.newBlock("lazy.more");
  uint32_t label = builder_.newLabel(block);
  builder_.repeatLabels_[pc] = label;
  builder_.b_.SetInsertPoint(block);
  BasicBlock *drop = builder_.newBlock("lazy.drop");
  Value *count = builder_.dataValue();
  Value *np = builder_.matcherChars().charStep(atom, builder_.load(builder_.value_), drop);
  if (builder_.memo_ && max == RE_INFINITY) {
    builder_.matcherMemo().memoCheck(builder_.repeatRow_[pc], pc, np, drop);
  }
  Value *counted = builder_.b_.CreateAdd(count, builder_.i32(1));
  BasicBlock *keep = builder_.newBlock("lazy.keep"), *last = builder_.newBlock("lazy.last"),
             *go = builder_.newBlock("lazy.resume");
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpULT(counted, builder_.i32(max)), keep, last);
  builder_.b_.SetInsertPoint(keep);
  Value *top = builder_.load(builder_.top_);
  builder_.putEntry(builder_.b_.CreateSub(top, builder_.i32(1)), builder_.i32(0), counted);
  builder_.putEntry(top, builder_.i32(label), np);
  builder_.store(builder_.b_.CreateAdd(top, builder_.i32(1)), builder_.top_);
  builder_.b_.CreateBr(go);
  builder_.b_.SetInsertPoint(last);
  builder_.store(builder_.b_.CreateSub(builder_.load(builder_.top_), builder_.i32(1)),
                 builder_.top_);
  builder_.b_.CreateBr(go);
  builder_.b_.SetInsertPoint(go);
  builder_.store(np, builder_.pos_);
  builder_.b_.CreateBr(builder_.blockAt(after));
  builder_.b_.SetInsertPoint(drop);
  builder_.store(builder_.b_.CreateSub(builder_.load(builder_.top_), builder_.i32(1)),
                 builder_.top_);
  builder_.b_.CreateBr(builder_.fail_);

  return label;
}

} // namespace tz
