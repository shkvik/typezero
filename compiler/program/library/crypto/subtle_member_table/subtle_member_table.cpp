#include "compiler/program/library/crypto/subtle_member_table/subtle_member_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kBytesExample = "new Uint8Array(16)";

const SubtleMemberInfo kMembers[] = {
    {.name = "name", .member = SubtleMember::NAME, .example = kBytesExample},
    {.name = "hash", .member = SubtleMember::HASH, .example = "\"SHA-256\""},
    {.name = "length", .member = SubtleMember::LENGTH, .example = "256"},
    {.name = "namedCurve", .member = SubtleMember::NAMED_CURVE, .example = "\"P-256\""},
    {.name = "iv", .member = SubtleMember::IV, .example = kBytesExample},
    {.name = "counter", .member = SubtleMember::COUNTER, .example = kBytesExample},
    {.name = "saltLength", .member = SubtleMember::SALT_LENGTH, .example = "32"},
    {.name = "modulusLength", .member = SubtleMember::MODULUS_LENGTH, .example = "2048"},
    {.name = "publicExponent",
     .member = SubtleMember::PUBLIC_EXPONENT,
     .example = "new Uint8Array([1, 0, 1])"},
    {.name = "iterations", .member = SubtleMember::ITERATIONS, .example = "600000"},
    {.name = "public", .member = SubtleMember::PEER_PUBLIC_KEY, .example = "theirPublicKey"},
    {.name = "tagLength", .member = SubtleMember::TAG_LENGTH, .example = kBytesExample},
};

}

llvm::StringRef SubtleMemberTable::getName(SubtleMember member) {
  for (const SubtleMemberInfo &info : kMembers) {
    if (info.member == member) {
      return info.name;
    }
  }
  llvm_unreachable("subtle member missing from the table");
}

llvm::StringRef SubtleMemberTable::getExample(llvm::StringRef name) {
  for (const SubtleMemberInfo &info : kMembers) {
    if (info.name == name) {
      return info.example;
    }
  }

  return kBytesExample;
}

}
