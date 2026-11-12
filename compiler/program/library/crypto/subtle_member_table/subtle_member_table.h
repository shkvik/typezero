#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

enum class SubtleMember {
  NAME,
  HASH,
  LENGTH,
  NAMED_CURVE,
  IV,
  COUNTER,
  SALT_LENGTH,
  MODULUS_LENGTH,
  PUBLIC_EXPONENT,
  ITERATIONS,
  PEER_PUBLIC_KEY,
  TAG_LENGTH,
};

struct SubtleMemberInfo {
  llvm::StringLiteral name;
  SubtleMember member;
  llvm::StringLiteral example;
};

class SubtleMemberTable {
public:
  static llvm::StringRef getName(SubtleMember member);
  static llvm::StringRef getExample(llvm::StringRef name);
};

}
