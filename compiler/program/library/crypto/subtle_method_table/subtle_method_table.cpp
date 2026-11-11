#include "compiler/program/library/crypto/subtle_method_table/subtle_method_table.h"

namespace tz {

namespace {

const SubtleMethodInfo kSubtleMethods[] = {
    {
        .name = "digest",
        .method = SubtleMethod::DIGEST,
    },
    {
        .name = "generateKey",
        .method = SubtleMethod::GENERATE_KEY,
    },
    {
        .name = "importKey",
        .method = SubtleMethod::IMPORT_KEY,
    },
    {
        .name = "exportKey",
        .method = SubtleMethod::EXPORT_KEY,
    },
    {
        .name = "sign",
        .method = SubtleMethod::SIGN,
    },
    {
        .name = "verify",
        .method = SubtleMethod::VERIFY,
    },
    {
        .name = "encrypt",
        .method = SubtleMethod::ENCRYPT,
    },
    {
        .name = "decrypt",
        .method = SubtleMethod::DECRYPT,
    },
    {
        .name = "deriveBits",
        .method = SubtleMethod::DERIVE_BITS,
    },
    {
        .name = "deriveKey",
        .method = SubtleMethod::DERIVE_KEY,
    },
    {
        .name = "wrapKey",
        .method = SubtleMethod::WRAP_KEY,
    },
    {
        .name = "unwrapKey",
        .method = SubtleMethod::UNWRAP_KEY,
    },
};

}

std::optional<SubtleMethod> SubtleMethodTable::find(llvm::StringRef name) {
  for (const SubtleMethodInfo &info : kSubtleMethods) {
    if (info.name == name) {
      return info.method;
    }
  }

  return std::nullopt;
}

}
