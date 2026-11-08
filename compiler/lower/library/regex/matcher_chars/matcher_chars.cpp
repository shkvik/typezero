#include "compiler/lower/library/regex/matcher_chars/matcher_chars.h"
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

MatcherChars::MatcherChars(MatcherBuilder &builder)
    : builder(builder) {}

Value *MatcherChars::test(Value *cond, const std::function<Value *()> &value) {
  BasicBlock *from = this->builder.b.GetInsertBlock();
  BasicBlock *yes = this->builder.newBlock("test"), *done = this->builder.newBlock("test.done");
  this->builder.b.CreateCondBr(cond, yes, done);
  this->builder.b.SetInsertPoint(yes);
  Value *v = value();
  BasicBlock *end = this->builder.b.GetInsertBlock();
  this->builder.b.CreateBr(done);
  this->builder.b.SetInsertPoint(done);
  llvm::PHINode *phi = this->builder.b.CreatePHI(this->builder.b.getInt1Ty(), 2);
  phi->addIncoming(this->builder.b.getFalse(), from);
  phi->addIncoming(v, end);

  return phi;
}

Value *MatcherChars::isLead(Value *u) {
  return this->builder.b.CreateICmpULE(this->builder.b.CreateSub(u, this->builder.i32(0xD800)),
                                       this->builder.i32(0x3FF));
}

Value *MatcherChars::isTrail(Value *u) {
  return this->builder.b.CreateICmpULE(this->builder.b.CreateSub(u, this->builder.i32(0xDC00)),
                                       this->builder.i32(0x3FF));
}

Value *MatcherChars::isLineTerminator(Value *c) {
  Value *lt = this->builder.b.CreateOr(this->builder.b.CreateICmpEQ(c, this->builder.i32('\n')),
                                       this->builder.b.CreateICmpEQ(c, this->builder.i32('\r')));
  if (this->builder.wide) {
    lt = this->builder.b.CreateOr(
        lt, this->builder.b.CreateICmpULE(this->builder.b.CreateSub(c, this->builder.i32(0x2028)),
                                          this->builder.i32(1)));
  }

  return lt;
}

Value *MatcherChars::isWordUnit(Value *u, bool folded) {
  Value *letter = this->builder.b.CreateICmpULE(
      this->builder.b.CreateSub(this->builder.b.CreateOr(u, this->builder.i32(0x20)),
                                this->builder.i32('a')),
      this->builder.i32(25));
  Value *digit = this->builder.b.CreateICmpULE(this->builder.b.CreateSub(u, this->builder.i32('0')),
                                               this->builder.i32(9));
  Value *word = this->builder.b.CreateOr(this->builder.b.CreateOr(letter, digit),
                                         this->builder.b.CreateICmpEQ(u, this->builder.i32('_')));
  if (folded && this->builder.wide) {
    word = this->builder.b.CreateOr(
        word, this->builder.b.CreateOr(this->builder.b.CreateICmpEQ(u, this->builder.i32(0x017F)),
                                       this->builder.b.CreateICmpEQ(u, this->builder.i32(0x212A))));
  }

  return word;
}

llvm::Constant *MatcherChars::byteTable(const ByteSet &bits) {
  llvm::Constant *&table = this->builder.tables[bits];
  if (table) {
    return table;
  }
  std::vector<uint8_t> bytes(256);
  for (uint32_t c = 0; c < 256; ++c) {
    bytes[c] = MatcherBuilder::hasByte(bits, c);
  }
  auto *data = llvm::ConstantDataArray::get(this->builder.ctx, bytes);
  auto *global =
      new llvm::GlobalVariable(this->builder.module_, data->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.class");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  table = global;

  return table;
}

Value *MatcherChars::inByteSet(Value *c, const ByteSet &bits) {
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  for (uint32_t x = 0; x < 256;) {
    if (!MatcherBuilder::hasByte(bits, x)) {
      ++x;
      continue;
    }
    uint32_t end = x;
    while (end + 1 < 256 && MatcherBuilder::hasByte(bits, end + 1)) {
      ++end;
    }
    runs.push_back({x, end});
    x = end + 1;
  }
  if (runs.empty()) {
    return this->builder.b.getFalse();
  }
  if (runs.size() <= 3) {
    Value *in = nullptr;
    for (auto [lo, hi] : runs) {
      Value *one =
          lo == hi
              ? this->builder.b.CreateICmpEQ(c, this->builder.i32(lo))
              : this->builder.b.CreateICmpULE(this->builder.b.CreateSub(c, this->builder.i32(lo)),
                                              this->builder.i32(hi - lo));
      in = in ? this->builder.b.CreateOr(in, one) : one;
    }
    return in;
  }
  Value *index = this->builder.b.CreateZExt(
      this->builder.b.CreateBinaryIntrinsic(llvm::Intrinsic::umin, c, this->builder.i32(255)),
      this->builder.b.getInt64Ty());
  Value *entry = this->builder.b.CreateLoad(
      this->builder.b.getInt8Ty(),
      this->builder.b.CreateInBoundsGEP(this->builder.b.getInt8Ty(), byteTable(bits), index));
  Value *in = this->builder.b.CreateICmpNE(entry, this->builder.b.getInt8(0));

  return this->builder.b.CreateAnd(in, this->builder.b.CreateICmpULT(c, this->builder.i32(256)));
}

Value *MatcherChars::inSet(Value *c, const uint32_t *set, bool bytesOnly) {
  ByteSet bits;
  for (int i = 0; i < 8; ++i) {
    bits[i] = set[1 + i];
  }
  Value *in = inByteSet(c, bits);
  uint32_t n = set[0];
  if (bytesOnly || n == 0) {
    return in;
  }
  const uint32_t *ranges = set + 9;
  if (n <= 4) {
    for (uint32_t i = 0; i < n; ++i) {
      in = this->builder.b.CreateOr(
          in, this->builder.b.CreateICmpULE(
                  this->builder.b.CreateSub(c, this->builder.i32(ranges[2 * i])),
                  this->builder.i32(ranges[2 * i + 1] - ranges[2 * i])));
    }
    return in;
  }
  std::vector<uint32_t> words(ranges, ranges + 2 * n);
  auto *data = llvm::ConstantDataArray::get(this->builder.ctx, words);
  auto *global =
      new llvm::GlobalVariable(this->builder.module_, data->getType(), true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.ranges");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::FunctionCallee fn = this->builder.module_.getOrInsertFunction(
      "tz_re_in_ranges",
      llvm::FunctionType::get(
          this->builder.b.getInt32Ty(),
          {this->builder.b.getInt32Ty(), this->builder.b.getPtrTy(), this->builder.b.getInt32Ty()},
          false));
  Value *high = test(this->builder.b.CreateICmpUGE(c, this->builder.i32(256)), [&] {
    return this->builder.b.CreateICmpNE(
        this->builder.b.CreateCall(fn, {c, global, this->builder.i32(n)}), this->builder.i32(0));
  });

  return this->builder.b.CreateOr(in, high);
}

std::pair<Value *, Value *> MatcherChars::readChar(Value *pos, bool back, BasicBlock *fail) {
  if (!this->builder.wide || !this->builder.unicode) {
    if (!back) {
      this->builder.failUnless(this->builder.b.CreateICmpSLT(pos, this->builder.length), fail);
      return {this->builder.unitAt(pos), this->builder.b.CreateAdd(pos, this->builder.i32(1))};
    }
    this->builder.failUnless(this->builder.b.CreateICmpSGT(pos, this->builder.i32(0)), fail);
    Value *before = this->builder.b.CreateSub(pos, this->builder.i32(1));
    return {this->builder.unitAt(before), before};
  }
  Value *at = pos;
  if (!back) {
    this->builder.failUnless(this->builder.b.CreateICmpSLT(pos, this->builder.length), fail);
  } else {
    this->builder.failUnless(this->builder.b.CreateICmpSGT(pos, this->builder.i32(0)), fail);
    at = this->builder.b.CreateSub(pos, this->builder.i32(1));
  }
  Value *c = this->builder.unitAt(at);
  BasicBlock *single = this->builder.b.GetInsertBlock(),
             *surrogate = this->builder.newBlock("surrogate"),
             *done = this->builder.newBlock("char");
  Value *singlePos = back ? at : this->builder.b.CreateAdd(pos, this->builder.i32(1));
  this->builder.b.CreateCondBr(
      this->builder.b.CreateICmpULE(this->builder.b.CreateSub(c, this->builder.i32(0xD800)),
                                    this->builder.i32(0x7FF)),
      surrogate, done);
  this->builder.b.SetInsertPoint(surrogate);
  llvm::Type *i64 = this->builder.b.getInt64Ty();
  llvm::FunctionCallee readFn = this->builder.module_.getOrInsertFunction(
      "tz_re_read_pair",
      llvm::FunctionType::get(i64,
                              {this->builder.b.getPtrTy(), this->builder.b.getInt32Ty(),
                               this->builder.b.getInt32Ty(), this->builder.b.getInt32Ty()},
                              false));
  Value *read = this->builder.b.CreateCall(
      readFn, {this->builder.units, this->builder.length, pos, this->builder.i32(back ? 1 : 0)});
  this->builder.failUnless(this->builder.b.CreateICmpSGE(read, this->builder.b.getInt64(0)), fail);
  Value *code = this->builder.b.CreateTrunc(read, this->builder.b.getInt32Ty());
  Value *pairPos = this->builder.b.CreateTrunc(
      this->builder.b.CreateLShr(read, this->builder.b.getInt64(32)), this->builder.b.getInt32Ty());
  BasicBlock *slow = this->builder.b.GetInsertBlock();
  this->builder.b.CreateBr(done);
  this->builder.b.SetInsertPoint(done);
  llvm::PHINode *cp = this->builder.b.CreatePHI(this->builder.b.getInt32Ty(), 2),
                *np = this->builder.b.CreatePHI(this->builder.b.getInt32Ty(), 2);
  cp->addIncoming(c, single);
  cp->addIncoming(code, slow);
  np->addIncoming(singlePos, single);
  np->addIncoming(pairPos, slow);

  return {cp, np};
}

Value *MatcherChars::charStep(uint32_t atom, Value *pos, BasicBlock *fail) {
  const uint32_t *op = this->builder.code + atom;
  bool back = MatcherBuilder::isBack(op[0]);
  if (op[0] == RE_CHAR || op[0] == RE_CHAR_BACK) {
    uint32_t c = op[1];
    if (!this->builder.wide && c > 255) {
      this->builder.b.CreateBr(fail ? fail : this->builder.fail);
      this->builder.b.SetInsertPoint(this->builder.newBlock("never"));
      return pos;
    }
    if (!this->builder.wide || !this->builder.unicode || c < 0xD800 ||
        (c > 0xDFFF && c < 0x10000)) {
      Value *at = pos;
      if (back) {
        this->builder.failUnless(this->builder.b.CreateICmpSGT(pos, this->builder.i32(0)), fail);
        at = this->builder.b.CreateSub(pos, this->builder.i32(1));
      } else {
        this->builder.failUnless(this->builder.b.CreateICmpSLT(pos, this->builder.length), fail);
      }
      this->builder.failUnless(
          this->builder.b.CreateICmpEQ(this->builder.unitAt(at), this->builder.i32(c)), fail);
      return back ? at : this->builder.b.CreateAdd(pos, this->builder.i32(1));
    }
    auto [cp, next] = readChar(pos, back, fail);
    this->builder.failUnless(this->builder.b.CreateICmpEQ(cp, this->builder.i32(c)), fail);
    return next;
  }
  auto [cp, next] = readChar(pos, back, fail);
  switch (op[0]) {
  case RE_ANY:
  case RE_ANY_BACK:
    this->builder.failUnless(this->builder.b.CreateNot(isLineTerminator(cp)), fail);
    break;
  case RE_SET:
  case RE_SET_BACK:
    this->builder.failUnless(inSet(cp, op + 1, !this->builder.wide), fail);
    break;
  default:
    break;
  }

  return next;
}

}
