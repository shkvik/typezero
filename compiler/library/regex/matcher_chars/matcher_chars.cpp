#include "compiler/library/regex/matcher_chars/matcher_chars.h"
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

bool isBack(uint32_t op) {
  return op == RE_CHAR_BACK || op == RE_ANY_BACK || op == RE_ALL_BACK || op == RE_SET_BACK;
}

bool hasByte(const ByteSet &s, uint32_t c) { return (s[c / 32] >> (c % 32)) & 1; }

} // namespace

MatcherChars::MatcherChars(MatcherBuilder &builder) : builder_(builder) {}

Value *MatcherChars::test(Value *cond, const std::function<Value *()> &value) {
  BasicBlock *from = builder_.b_.GetInsertBlock();
  BasicBlock *yes = builder_.newBlock("test"), *done = builder_.newBlock("test.done");
  builder_.b_.CreateCondBr(cond, yes, done);
  builder_.b_.SetInsertPoint(yes);
  Value *v = value();
  BasicBlock *end = builder_.b_.GetInsertBlock();
  builder_.b_.CreateBr(done);
  builder_.b_.SetInsertPoint(done);
  llvm::PHINode *phi = builder_.b_.CreatePHI(builder_.b_.getInt1Ty(), 2);
  phi->addIncoming(builder_.b_.getFalse(), from);
  phi->addIncoming(v, end);

  return phi;
}

Value *MatcherChars::isLead(Value *u) {
  return builder_.b_.CreateICmpULE(builder_.b_.CreateSub(u, builder_.i32(0xD800)), builder_.i32(0x3FF));
}

Value *MatcherChars::isTrail(Value *u) {
  return builder_.b_.CreateICmpULE(builder_.b_.CreateSub(u, builder_.i32(0xDC00)), builder_.i32(0x3FF));
}

Value *MatcherChars::isLineTerminator(Value *c) {
  Value *lt = builder_.b_.CreateOr(builder_.b_.CreateICmpEQ(c, builder_.i32('\n')), builder_.b_.CreateICmpEQ(c, builder_.i32('\r')));
  if (builder_.wide_) {
    lt = builder_.b_.CreateOr(lt, builder_.b_.CreateICmpULE(builder_.b_.CreateSub(c, builder_.i32(0x2028)), builder_.i32(1)));
  }

  return lt;
}

Value *MatcherChars::isWordUnit(Value *u, bool folded) {
  Value *letter = builder_.b_.CreateICmpULE(builder_.b_.CreateSub(builder_.b_.CreateOr(u, builder_.i32(0x20)), builder_.i32('a')), builder_.i32(25));
  Value *digit = builder_.b_.CreateICmpULE(builder_.b_.CreateSub(u, builder_.i32('0')), builder_.i32(9));
  Value *word = builder_.b_.CreateOr(builder_.b_.CreateOr(letter, digit), builder_.b_.CreateICmpEQ(u, builder_.i32('_')));
  if (folded && builder_.wide_) {
    word = builder_.b_.CreateOr(
        word, builder_.b_.CreateOr(builder_.b_.CreateICmpEQ(u, builder_.i32(0x017F)), builder_.b_.CreateICmpEQ(u, builder_.i32(0x212A))));
  }

  return word;
}

llvm::Constant *MatcherChars::byteTable(const ByteSet &bits) {
  llvm::Constant *&table = builder_.tables_[bits];
  if (table) {
    return table;
  }
  std::vector<uint8_t> bytes(256);
  for (uint32_t c = 0; c < 256; ++c) {
    bytes[c] = hasByte(bits, c);
  }
  auto *data = llvm::ConstantDataArray::get(builder_.ctx_, bytes);
  auto *global =
      new llvm::GlobalVariable(builder_.module_, data->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.class");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  table = global;

  return table;
}

// Whether `c` (below 256) is in `bits`: a few compares, or a table.
Value *MatcherChars::inByteSet(Value *c, const ByteSet &bits) {
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  for (uint32_t x = 0; x < 256;) {
    if (!hasByte(bits, x)) {
      ++x;
      continue;
    }
    uint32_t end = x;
    while (end + 1 < 256 && hasByte(bits, end + 1)) {
      ++end;
    }
    runs.push_back({x, end});
    x = end + 1;
  }
  if (runs.empty()) {
    return builder_.b_.getFalse();
  }
  if (runs.size() <= 3) {
    Value *in = nullptr;
    for (auto [lo, hi] : runs) {
      Value *one = lo == hi ? builder_.b_.CreateICmpEQ(c, builder_.i32(lo))
                            : builder_.b_.CreateICmpULE(builder_.b_.CreateSub(c, builder_.i32(lo)), builder_.i32(hi - lo));
      in = in ? builder_.b_.CreateOr(in, one) : one;
    }
    return in;
  }
  Value *index =
      builder_.b_.CreateZExt(builder_.b_.CreateBinaryIntrinsic(llvm::Intrinsic::umin, c, builder_.i32(255)), builder_.b_.getInt64Ty());
  Value *entry =
      builder_.b_.CreateLoad(builder_.b_.getInt8Ty(), builder_.b_.CreateInBoundsGEP(builder_.b_.getInt8Ty(), byteTable(bits), index));
  Value *in = builder_.b_.CreateICmpNE(entry, builder_.b_.getInt8(0));

  return builder_.b_.CreateAnd(in, builder_.b_.CreateICmpULT(c, builder_.i32(256)));
}

// RE_SET's operands at `set`: n, 8 words of bits, n ranges from 256 on.
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
      in = builder_.b_.CreateOr(in, builder_.b_.CreateICmpULE(builder_.b_.CreateSub(c, builder_.i32(ranges[2 * i])),
                                            builder_.i32(ranges[2 * i + 1] - ranges[2 * i])));
    }
    return in;
  }
  std::vector<uint32_t> words(ranges, ranges + 2 * n);
  auto *data = llvm::ConstantDataArray::get(builder_.ctx_, words);
  auto *global =
      new llvm::GlobalVariable(builder_.module_, data->getType(), /*isConstant=*/true,
                               llvm::GlobalValue::PrivateLinkage, data, "tz.regex.ranges");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  llvm::FunctionCallee fn = builder_.module_.getOrInsertFunction(
      "tz_re_in_ranges",
      llvm::FunctionType::get(builder_.b_.getInt32Ty(), {builder_.b_.getInt32Ty(), builder_.b_.getPtrTy(), builder_.b_.getInt32Ty()},
                              false));
  // Below 256 the bits answer; the call is for the rest only.
  Value *high = test(builder_.b_.CreateICmpUGE(c, builder_.i32(256)), [&] {
    return builder_.b_.CreateICmpNE(builder_.b_.CreateCall(fn, {c, global, builder_.i32(n)}), builder_.i32(0));
  });

  return builder_.b_.CreateOr(in, high);
}

std::pair<Value *, Value *> MatcherChars::readChar(Value *pos, bool back, BasicBlock *fail) {
  if (!builder_.wide_ || !builder_.unicode_) {
    if (!back) {
      builder_.failUnless(builder_.b_.CreateICmpSLT(pos, builder_.length_), fail);
      return {builder_.unitAt(pos), builder_.b_.CreateAdd(pos, builder_.i32(1))};
    }
    builder_.failUnless(builder_.b_.CreateICmpSGT(pos, builder_.i32(0)), fail);
    Value *before = builder_.b_.CreateSub(pos, builder_.i32(1));
    return {builder_.unitAt(before), before};
  }
  // With u, a surrogate pair is one character, and in the middle of one there
  // is none (see read_forward in runtime/library/regex/regex/regex.c): the
  // runtime reads it, as a unit that is not a surrogate is the character
  // itself.
  Value *at = pos;
  if (!back) {
    builder_.failUnless(builder_.b_.CreateICmpSLT(pos, builder_.length_), fail);
  } else {
    builder_.failUnless(builder_.b_.CreateICmpSGT(pos, builder_.i32(0)), fail);
    at = builder_.b_.CreateSub(pos, builder_.i32(1));
  }
  Value *c = builder_.unitAt(at);
  BasicBlock *single = builder_.b_.GetInsertBlock(), *surrogate = builder_.newBlock("surrogate"),
             *done = builder_.newBlock("char");
  Value *singlePos = back ? at : builder_.b_.CreateAdd(pos, builder_.i32(1));
  builder_.b_.CreateCondBr(builder_.b_.CreateICmpULE(builder_.b_.CreateSub(c, builder_.i32(0xD800)), builder_.i32(0x7FF)), surrogate, done);
  builder_.b_.SetInsertPoint(surrogate);
  llvm::Type *i64 = builder_.b_.getInt64Ty();
  llvm::FunctionCallee readFn = builder_.module_.getOrInsertFunction(
      "tz_re_read_pair",
      llvm::FunctionType::get(
          i64, {builder_.b_.getPtrTy(), builder_.b_.getInt32Ty(), builder_.b_.getInt32Ty(), builder_.b_.getInt32Ty()}, false));
  Value *read = builder_.b_.CreateCall(readFn, {builder_.units_, builder_.length_, pos, builder_.i32(back ? 1 : 0)});
  builder_.failUnless(builder_.b_.CreateICmpSGE(read, builder_.b_.getInt64(0)), fail);
  Value *code = builder_.b_.CreateTrunc(read, builder_.b_.getInt32Ty());
  Value *pairPos = builder_.b_.CreateTrunc(builder_.b_.CreateLShr(read, builder_.b_.getInt64(32)), builder_.b_.getInt32Ty());
  BasicBlock *slow = builder_.b_.GetInsertBlock();
  builder_.b_.CreateBr(done);
  builder_.b_.SetInsertPoint(done);
  llvm::PHINode *cp = builder_.b_.CreatePHI(builder_.b_.getInt32Ty(), 2), *np = builder_.b_.CreatePHI(builder_.b_.getInt32Ty(), 2);
  cp->addIncoming(c, single);
  cp->addIncoming(code, slow);
  np->addIncoming(singlePos, single);
  np->addIncoming(pairPos, slow);

  return {cp, np};
}

// The one-character instruction at `atom` at `pos`: the position past (or
// before) the character, or a jump to `fail`.
Value *MatcherChars::charStep(uint32_t atom, Value *pos, BasicBlock *fail) {
  const uint32_t *op = builder_.code_ + atom;
  bool back = isBack(op[0]);
  if (op[0] == RE_CHAR || op[0] == RE_CHAR_BACK) {
    uint32_t c = op[1];
    if (!builder_.wide_ && c > 255) {
      builder_.b_.CreateBr(fail ? fail : builder_.fail_);
      builder_.b_.SetInsertPoint(builder_.newBlock("never"));
      return pos;
    }
    if (!builder_.wide_ || !builder_.unicode_ || c < 0xD800 || (c > 0xDFFF && c < 0x10000)) {
      // A code unit: no pairs to think of.
      Value *at = pos;
      if (back) {
        builder_.failUnless(builder_.b_.CreateICmpSGT(pos, builder_.i32(0)), fail);
        at = builder_.b_.CreateSub(pos, builder_.i32(1));
      } else {
        builder_.failUnless(builder_.b_.CreateICmpSLT(pos, builder_.length_), fail);
      }
      builder_.failUnless(builder_.b_.CreateICmpEQ(builder_.unitAt(at), builder_.i32(c)), fail);
      return back ? at : builder_.b_.CreateAdd(pos, builder_.i32(1));
    }
    auto [cp, next] = readChar(pos, back, fail);
    builder_.failUnless(builder_.b_.CreateICmpEQ(cp, builder_.i32(c)), fail);
    return next;
  }
  auto [cp, next] = readChar(pos, back, fail);
  switch (op[0]) {
  case RE_ANY:
  case RE_ANY_BACK:
    builder_.failUnless(builder_.b_.CreateNot(isLineTerminator(cp)), fail);
    break;
  case RE_SET:
  case RE_SET_BACK:
    builder_.failUnless(inSet(cp, op + 1, !builder_.wide_), fail);
    break;
  default: // RE_ALL
    break;
  }

  return next;
}

} // namespace tz
