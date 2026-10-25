#include "compiler/library/regex/regex_native_lowerer/regex_native_lowerer.h"
#include "compiler/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
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

bool isCharInstruction(uint32_t op) {
  return op == RE_CHAR || op == RE_ANY || op == RE_ALL || op == RE_SET || isBack(op);
}

bool hasByte(const ByteSet &s, uint32_t c) { return (s[c / 32] >> (c % 32)) & 1; }

void addByte(ByteSet &s, uint32_t c) { s[c / 32] |= 1u << (c % 32); }

uint64_t counterRange(const CountedLoop &loop) {
  return loop.max == RE_INFINITY ? uint64_t(loop.min) + 1 : uint64_t(loop.max) + 1;
}

} // namespace

RegexNativeLowerer::RegexNativeLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Constant *RegexNativeLowerer::regexMatcher(const RegexLitExpr *lit) {
  llvm::Constant *&matcher = regexMatchers_[lit->program];
  if (matcher) {
    return matcher;
  }
  MatcherBuilder builder(*lowering_.module_, lit->program.data(), lowering_.parallel_);
  matcher = builder.build();
  if (!matcher) {
    matcher = cast<llvm::Function>(
        lowering_.regexLowerer()
            .regexFunction("tz_re_interpret", lowering_.builder_.getInt32Ty(),
                           {lowering_.builder_.getPtrTy(), lowering_.builder_.getPtrTy(),
                            lowering_.builder_.getInt32Ty(), lowering_.builder_.getInt32Ty(),
                            lowering_.builder_.getInt32Ty(), lowering_.builder_.getPtrTy(),
                            lowering_.builder_.getInt32Ty(), lowering_.builder_.getInt32Ty()})
            .getCallee());
  }

  return matcher;
}

} // namespace tz
