#include "compiler/lower/library/regex/matcher_memo/matcher_memo.h"
#include "compiler/lower/library/regex/matcher_analysis/matcher_analysis.h"
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

MatcherMemo::MatcherMemo(MatcherBuilder &builder)
    : builder(builder) {}

Value *MatcherMemo::memoContext(uint32_t pc, Value *pos) {
  Value *context = this->builder.i32(0);
  for (const CountedLoop &loop : this->builder.loops) {
    if (!this->builder.matcherAnalysis().inLoop(loop, pc)) {
      continue;
    }
    uint32_t range = uint32_t(MatcherBuilder::counterRange(loop));
    Value *n = this->builder.b.CreateBinaryIntrinsic(
        llvm::Intrinsic::umin, this->builder.loadSlot(2 * this->builder.captures + loop.counter),
        this->builder.i32(range - 1));
    context =
        this->builder.b.CreateAdd(this->builder.b.CreateMul(context, this->builder.i32(range)), n);
    if (this->builder.matcherAnalysis().flagged(loop, pc)) {
      Value *matched = this->builder.b.CreateICmpNE(
          pos, this->builder.loadSlot(2 * this->builder.captures + loop.position));
      context = this->builder.b.CreateAdd(
          this->builder.b.CreateMul(context, this->builder.i32(2)),
          this->builder.b.CreateZExt(matched, this->builder.b.getInt32Ty()));
    }
  }

  return context;
}

llvm::StructType *MatcherMemo::memoType() {
  llvm::Type *ptr = this->builder.b.getPtrTy(), *i32Ty = this->builder.b.getInt32Ty();

  return llvm::StructType::get(this->builder.ctx, {ptr, i32Ty, i32Ty, ptr, i32Ty, i32Ty});
}

void MatcherMemo::budgetCheck() {
  if (!this->builder.memo) {
    return;
  }
  llvm::Type *i64 = this->builder.b.getInt64Ty();
  Value *starts = this->builder.b.CreateSExt(
      this->builder.b.CreateSub(this->builder.load(this->builder.attempt),
                                this->builder.load(this->builder.search)),
      i64);
  Value *budget =
      this->builder.b.CreateAdd(this->builder.b.CreateMul(starts, this->builder.b.getInt64(16)),
                                this->builder.b.getInt64(1024));
  BasicBlock *over = this->builder.newBlock("memo.over"),
             *start = this->builder.newBlock("memo.start"), *go = this->builder.newBlock("memo.go");
  this->builder.b.CreateCondBr(
      this->builder.b.CreateICmpSGE(
          this->builder.b.CreateSExt(this->builder.load(this->builder.steps), i64), budget),
      over, go);
  this->builder.b.SetInsertPoint(over);
  this->builder.store(this->builder.i32(0x80000000u), this->builder.steps);
  this->builder.b.CreateCondBr(
      this->builder.b.CreateIsNull(this->builder.load(this->builder.memoAt)), start, go);
  this->builder.b.SetInsertPoint(start);
  llvm::FunctionCallee startFn = this->builder.module_.getOrInsertFunction(
      "tz_re_memo_start",
      llvm::FunctionType::get(this->builder.b.getPtrTy(),
                              {this->builder.b.getInt32Ty(), this->builder.b.getInt32Ty()}, false));
  this->builder.store(this->builder.b.CreateCall(
                          startFn, {this->builder.i32(this->builder.rows), this->builder.length}),
                      this->builder.memoAt);
  this->builder.b.CreateBr(go);
  this->builder.b.SetInsertPoint(go);
}

void MatcherMemo::countWork(Value *from, Value *to) {
  if (!this->builder.memo) {
    return;
  }
  Value *read = this->builder.b.CreateSub(to, from);
  read = this->builder.b.CreateSelect(this->builder.b.CreateICmpSLT(read, this->builder.i32(0)),
                                      this->builder.b.CreateNeg(read), read);
  this->builder.store(this->builder.b.CreateAdd(this->builder.load(this->builder.steps), read),
                      this->builder.steps);
}

void MatcherMemo::memoCheck(uint32_t row, uint32_t pc, Value *pos, BasicBlock *visited) {
  Value *memo = this->builder.load(this->builder.memoAt);
  BasicBlock *on = this->builder.newBlock("memo"), *done = this->builder.newBlock("memo.done");
  this->builder.b.CreateCondBr(this->builder.b.CreateIsNotNull(memo), on, done);
  this->builder.b.SetInsertPoint(on);
  Value *log = this->builder.hasLook
                   ? this->builder.b.CreateZExt(
                         this->builder.b.CreateICmpNE(this->builder.load(this->builder.depth),
                                                      this->builder.i32(0)),
                         this->builder.b.getInt32Ty())
                   : this->builder.i32(0);
  llvm::Type *i32Ty = this->builder.b.getInt32Ty();
  llvm::FunctionCallee visit = this->builder.module_.getOrInsertFunction(
      "tz_re_memo_visit",
      llvm::FunctionType::get(i32Ty, {this->builder.b.getPtrTy(), i32Ty, i32Ty, i32Ty}, false));
  Value *seen = this->builder.b.CreateCall(
      visit,
      {memo, this->builder.b.CreateAdd(this->builder.i32(row), memoContext(pc, pos)), pos, log});
  this->builder.b.CreateCondBr(this->builder.b.CreateICmpNE(seen, this->builder.i32(0)), visited,
                               done);
  this->builder.b.SetInsertPoint(done);
}

Value *MatcherMemo::memoLogLength() {
  Value *memo = this->builder.load(this->builder.memoAt);
  BasicBlock *from = this->builder.b.GetInsertBlock(), *on = this->builder.newBlock("log"),
             *done = this->builder.newBlock("log.done");
  this->builder.b.CreateCondBr(this->builder.b.CreateIsNotNull(memo), on, done);
  this->builder.b.SetInsertPoint(on);
  Value *length =
      this->builder.b.CreateLoad(this->builder.b.getInt32Ty(),
                                 this->builder.b.CreateStructGEP(memoType(), memo, kMemoLogLength));
  this->builder.b.CreateBr(done);
  this->builder.b.SetInsertPoint(done);
  llvm::PHINode *phi = this->builder.b.CreatePHI(this->builder.b.getInt32Ty(), 2);
  phi->addIncoming(this->builder.i32(0), from);
  phi->addIncoming(length, on);

  return phi;
}

void MatcherMemo::withMemo(llvm::StringRef name, llvm::ArrayRef<Value *> args) {
  Value *memo = this->builder.load(this->builder.memoAt);
  BasicBlock *on = this->builder.newBlock("memo.call"),
             *done = this->builder.newBlock("memo.called");
  this->builder.b.CreateCondBr(this->builder.b.CreateIsNotNull(memo), on, done);
  this->builder.b.SetInsertPoint(on);
  std::vector<llvm::Type *> types = {this->builder.b.getPtrTy()};
  std::vector<Value *> values = {memo};
  for (Value *arg : args) {
    types.push_back(arg->getType());
    values.push_back(arg);
  }
  llvm::FunctionCallee fn = this->builder.module_.getOrInsertFunction(
      name, llvm::FunctionType::get(this->builder.b.getVoidTy(), types, false));
  this->builder.b.CreateCall(fn, values);
  this->builder.b.CreateBr(done);
  this->builder.b.SetInsertPoint(done);
}

}
