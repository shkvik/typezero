#include "compiler/program/library/math/bit_wrap_table/bit_wrap_table.h"

namespace tz {

namespace {

const BitWrapInfo kBitWraps[] = {
    {
        .name = "Wrap32",
        .bits = 32,
    },
    {
        .name = "Wrap64",
        .bits = 64,
    },
};

const BitWrapMemberInfo kBitWrapMembers[] = {
    {
        .name = "from",
        .member = BitWrapMember::FROM,
    },
};

}

std::optional<unsigned> BitWrapTable::findBits(llvm::StringRef name) {
  for (const BitWrapInfo &info : kBitWraps) {
    if (info.name == name) {
      return info.bits;
    }
  }

  return std::nullopt;
}

std::optional<BitWrapMember> BitWrapTable::findMember(llvm::StringRef name) {
  for (const BitWrapMemberInfo &info : kBitWrapMembers) {
    if (info.name == name) {
      return info.member;
    }
  }

  return std::nullopt;
}

}
