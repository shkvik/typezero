#include "compiler/library/regex/matcher_memo/matcher_memo.h"
#include "compiler/library/regex/matcher_analysis/matcher_analysis.h"
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

namespace {

uint64_t counterRange(const CountedLoop &loop) {
  return loop.max == RE_INFINITY ? uint64_t(loop.min) + 1 : uint64_t(loop.max) + 1;
}

} // namespace

MatcherMemo::MatcherMemo(MatcherBuilder &builder) : builder_(builder) {}

// Which state of the counted loops around `pc` the match is in.
Value *MatcherMemo::memoContext(uint32_t pc, Value *pos) {
  Value *context = builder_.i32(0);
  for (const CountedLoop &loop : builder_.loops_) {
    if (!builder_.matcherAnalysis().inLoop(loop, pc)) {
      continue;
    }
    uint32_t range = uint32_t(counterRange(loop));
    Value *n = builder_.b_.CreateBinaryIntrinsic(llvm::Intrinsic::umin,
                                        builder_.loadSlot(2 * builder_.captures_ + loop.counter), builder_.i32(range - 1));
    context = builder_.b_.CreateAdd(builder_.b_.CreateMul(context, builder_.i32(range)), n);
    if (builder_.matcherAnalysis().flagged(loop, pc)) {
      Value *matched = builder_.b_.CreateICmpNE(pos, builder_.loadSlot(2 * builder_.captures_ + loop.position));
      context =
          builder_.b_.CreateAdd(builder_.b_.CreateMul(context, builder_.i32(2)), builder_.b_.CreateZExt(matched, builder_.b_.getInt32Ty()));
    }
  }

  return context;
}

llvm::StructType *MatcherMemo::memoType() {
  llvm::Type *ptr = builder_.b_.getPtrTy(), *i32Ty = builder_.b_.getInt32Ty();

  return llvm::StructType::get(builder_.ctx_, {ptr, i32Ty, i32Ty, ptr, i32Ty, i32Ty});
}

// Past the budget of work, the memo goes on. The work is the choices taken,
// the iterations of loops and the characters repeats read; the budget grows
// with the starts the search went past, so that a search that backtracks
// within bounds never needs the memo.
void MatcherMemo::budgetCheck() {
  if (!builder_.memo_) {
    return;
  }
  llvm::Type *i64 = builder_.b_.getInt64Ty();
  Value *starts = builder_.b_.CreateSExt(builder_.b_.CreateSub(builder_.load(builder_.attempt_), builder_.load(builder_.search_)), i64);
  Value *budget = builder_.b_.CreateAdd(builder_.b_.CreateMul(starts, builder_.b_.getInt64(16)), builder_.b_.getInt64(1024));
  BasicBlock *over = builder_.newBlock("memo.over"), *start = builder_.newBlock("memo.start"),
             *go = builder_.newBlock("memo.go");
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpSGE(builder_.b_.CreateSExt(builder_.load(builder_.steps_), i64), budget), over, go);
  builder_.b_.SetInsertPoint(over);
  builder_.store(builder_.i32(0x80000000u), builder_.steps_); // not again
  builder_.b_.CreateCondBr(builder_.b_.CreateIsNull(builder_.load(builder_.memoAt_)), start, go);
  builder_.b_.SetInsertPoint(start);
  llvm::FunctionCallee startFn = builder_.module_.getOrInsertFunction(
      "tz_re_memo_start",
      llvm::FunctionType::get(builder_.b_.getPtrTy(), {builder_.b_.getInt32Ty(), builder_.b_.getInt32Ty()}, false));
  builder_.store(builder_.b_.CreateCall(startFn, {builder_.i32(builder_.rows_), builder_.length_}), builder_.memoAt_);
  builder_.b_.CreateBr(go);
  builder_.b_.SetInsertPoint(go);
}

// Adds what a repeat read, from `from` to `to`, to the work.
void MatcherMemo::countWork(Value *from, Value *to) {
  if (!builder_.memo_) {
    return;
  }
  Value *read = builder_.b_.CreateSub(to, from);
  read = builder_.b_.CreateSelect(builder_.b_.CreateICmpSLT(read, builder_.i32(0)), builder_.b_.CreateNeg(read), read);
  builder_.store(builder_.b_.CreateAdd(builder_.load(builder_.steps_), read), builder_.steps_);
}

// Notes that the match got to row `row` (and the loop state of `pc`) at
// `pos`, or goes to `visited` if it got there before. Only with the memo on.
void MatcherMemo::memoCheck(uint32_t row, uint32_t pc, Value *pos, BasicBlock *visited) {
  Value *memo = builder_.load(builder_.memoAt_);
  BasicBlock *on = builder_.newBlock("memo"), *done = builder_.newBlock("memo.done");
  builder_.b_.CreateCondBr(builder_.b_.CreateIsNotNull(memo), on, done);
  builder_.b_.SetInsertPoint(on);
  // Inside a lookaround what is noted may lie on the way to its match,
  // which is no failure: noted down too, to be forgotten if it matches.
  Value *log =
      builder_.hasLook_ ? builder_.b_.CreateZExt(builder_.b_.CreateICmpNE(builder_.load(builder_.depth_), builder_.i32(0)), builder_.b_.getInt32Ty()) : builder_.i32(0);
  llvm::Type *i32Ty = builder_.b_.getInt32Ty();
  llvm::FunctionCallee visit = builder_.module_.getOrInsertFunction(
      "tz_re_memo_visit",
      llvm::FunctionType::get(i32Ty, {builder_.b_.getPtrTy(), i32Ty, i32Ty, i32Ty}, false));
  Value *seen =
      builder_.b_.CreateCall(visit, {memo, builder_.b_.CreateAdd(builder_.i32(row), memoContext(pc, pos)), pos, log});
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpNE(seen, builder_.i32(0)), visited, done);
  builder_.b_.SetInsertPoint(done);
}

// How long the memo's log is (0 without the memo).
Value *MatcherMemo::memoLogLength() {
  Value *memo = builder_.load(builder_.memoAt_);
  BasicBlock *from = builder_.b_.GetInsertBlock(), *on = builder_.newBlock("log"), *done = builder_.newBlock("log.done");
  builder_.b_.CreateCondBr(builder_.b_.CreateIsNotNull(memo), on, done);
  builder_.b_.SetInsertPoint(on);
  Value *length =
      builder_.b_.CreateLoad(builder_.b_.getInt32Ty(), builder_.b_.CreateStructGEP(memoType(), memo, kMemoLogLength));
  builder_.b_.CreateBr(done);
  builder_.b_.SetInsertPoint(done);
  llvm::PHINode *phi = builder_.b_.CreatePHI(builder_.b_.getInt32Ty(), 2);
  phi->addIncoming(builder_.i32(0), from);
  phi->addIncoming(length, on);

  return phi;
}

// Calls `name` with the memo first, if there is one.
void MatcherMemo::withMemo(llvm::StringRef name, llvm::ArrayRef<Value *> args) {
  Value *memo = builder_.load(builder_.memoAt_);
  BasicBlock *on = builder_.newBlock("memo.call"), *done = builder_.newBlock("memo.called");
  builder_.b_.CreateCondBr(builder_.b_.CreateIsNotNull(memo), on, done);
  builder_.b_.SetInsertPoint(on);
  std::vector<llvm::Type *> types = {builder_.b_.getPtrTy()};
  std::vector<Value *> values = {memo};
  for (Value *arg : args) {
    types.push_back(arg->getType());
    values.push_back(arg);
  }
  llvm::FunctionCallee fn =
      builder_.module_.getOrInsertFunction(name, llvm::FunctionType::get(builder_.b_.getVoidTy(), types, false));
  builder_.b_.CreateCall(fn, values);
  builder_.b_.CreateBr(done);
  builder_.b_.SetInsertPoint(done);
}

} // namespace tz
