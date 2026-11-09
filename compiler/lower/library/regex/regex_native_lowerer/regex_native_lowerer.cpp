#include "compiler/lower/library/regex/regex_native_lowerer/regex_native_lowerer.h"
#include "compiler/lower/library/regex/matcher_builder/matcher_builder.h"
#include "compiler/lower/library/regex/regex_lowerer/regex_lowerer.h"
#include "runtime/library/regex/regex/regex.h"

using llvm::cast;

namespace tz {

RegexNativeLowerer::RegexNativeLowerer(Lowering &owner)
    : lowering(owner) {}

bool RegexNativeLowerer::isBack(uint32_t op) {
  return op == RE_CHAR_BACK || op == RE_ANY_BACK || op == RE_ALL_BACK || op == RE_SET_BACK;
}

bool RegexNativeLowerer::isCharInstruction(uint32_t op) {
  return op == RE_CHAR || op == RE_ANY || op == RE_ALL || op == RE_SET ||
         RegexNativeLowerer::isBack(op);
}

bool RegexNativeLowerer::hasByte(const ByteSet &s, uint32_t c) {
  return (s[c / 32] >> (c % 32)) & 1;
}

void RegexNativeLowerer::addByte(ByteSet &s, uint32_t c) {
  s[c / 32] |= 1u << (c % 32);
}

uint64_t RegexNativeLowerer::counterRange(const CountedLoop &loop) {
  return loop.max == RE_INFINITY ? uint64_t(loop.min) + 1 : uint64_t(loop.max) + 1;
}

llvm::Constant *RegexNativeLowerer::regexMatcher(const RegexLitExpr *lit) {
  llvm::Constant *&matcher = this->regexMatchers[lit->program];
  if (matcher) {
    return matcher;
  }
  MatcherBuilder builder(*this->lowering.module_, lit->program.data(), this->lowering.parallel_);
  matcher = builder.build();
  if (!matcher) {
    matcher = cast<llvm::Function>(
        this->lowering.regexLowerer()
            .regexFunction(
                "tz_re_interpret", this->lowering.builder.getInt32Ty(),
                {this->lowering.builder.getPtrTy(), this->lowering.builder.getPtrTy(),
                 this->lowering.builder.getInt32Ty(), this->lowering.builder.getInt32Ty(),
                 this->lowering.builder.getInt32Ty(), this->lowering.builder.getPtrTy(),
                 this->lowering.builder.getInt32Ty(), this->lowering.builder.getInt32Ty()})
            .getCallee());
  }

  return matcher;
}

}
