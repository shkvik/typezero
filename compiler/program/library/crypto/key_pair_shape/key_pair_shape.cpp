#include "compiler/program/library/crypto/key_pair_shape/key_pair_shape.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kTypeName = "KeyPair";

const KeyPairFieldInfo kFields[] = {
    {.name = "publicKey", .field = KeyPairField::PUBLIC_KEY},
    {.name = "privateKey", .field = KeyPairField::PRIVATE_KEY},
};

}

llvm::StringRef KeyPairShape::getTypeName() {
  return kTypeName;
}

llvm::StringRef KeyPairShape::getFieldName(KeyPairField field) {
  for (const KeyPairFieldInfo &info : kFields) {
    if (info.field == field) {
      return info.name;
    }
  }
  llvm_unreachable("key pair field missing from the table");
}

}
