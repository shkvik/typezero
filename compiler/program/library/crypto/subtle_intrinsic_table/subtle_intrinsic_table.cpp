#include "compiler/program/library/crypto/subtle_intrinsic_table/subtle_intrinsic_table.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kSubtlePrefix = "subtle";

const SubtleIntrinsic kSubtleIntrinsics[] = {
    {"subtleDigest", CryptoFn::SubtleDigest, "md", 'B'},
    {"subtleHmac", CryptoFn::SubtleHmac, "mdd", 'B'},
    {"subtleHmacVerify", CryptoFn::SubtleHmacVerify, "mddd", 'I'},
    {"subtlePbkdf2", CryptoFn::SubtlePbkdf2, "mddff", 'B'},
    {"subtleHkdf", CryptoFn::SubtleHkdf, "mdddf", 'U'},
    {"subtleAes", CryptoFn::SubtleAes, "cdbddiid", 'U'},
    {"subtleAesKw", CryptoFn::SubtleAesKw, "dbd", 'U'},
    {"subtleSign", CryptoFn::SubtleSign, "oisid", 'U'},
    {"subtleVerify", CryptoFn::SubtleVerify, "oisidd", 'I'},
    {"subtleRsaOaep", CryptoFn::SubtleRsaOaep, "osdbd", 'U'},
    {"subtleDerive", CryptoFn::SubtleDerive, "oo", 'U'},
    {"subtleGenerate", CryptoFn::SubtleGenerate, "iffs", 'K'},
};

}

const SubtleIntrinsic *SubtleIntrinsicTable::find(llvm::StringRef name) {
  for (const SubtleIntrinsic &intrinsic : kSubtleIntrinsics) {
    if (intrinsic.name == name) {
      return &intrinsic;
    }
  }

  return nullptr;
}

bool SubtleIntrinsicTable::isSubtleName(llvm::StringRef name) {
  return name.starts_with(kSubtlePrefix);
}

}
