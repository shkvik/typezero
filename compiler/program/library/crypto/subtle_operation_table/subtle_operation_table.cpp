#include "compiler/program/library/crypto/subtle_operation_table/subtle_operation_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const SubtleOperationInfo kRows[] = {
    {
        .name = "digest",
        .value = SubtleOperation::DIGEST,
    },
    {
        .name = "generateKey",
        .value = SubtleOperation::GENERATE_KEY,
    },
    {
        .name = "importKey",
        .value = SubtleOperation::IMPORT_KEY,
    },
    {
        .name = "encrypt",
        .value = SubtleOperation::ENCRYPT,
    },
    {
        .name = "decrypt",
        .value = SubtleOperation::DECRYPT,
    },
    {
        .name = "sign",
        .value = SubtleOperation::SIGN,
    },
    {
        .name = "verify",
        .value = SubtleOperation::VERIFY,
    },
    {
        .name = "deriveBits",
        .value = SubtleOperation::DERIVE_BITS,
    },
    {
        .name = "wrapKey",
        .value = SubtleOperation::WRAP_KEY,
    },
    {
        .name = "unwrapKey",
        .value = SubtleOperation::UNWRAP_KEY,
    },
    {
        .name = "get key length",
        .value = SubtleOperation::KEY_LENGTH,
    },
};

}

std::optional<SubtleOperation> SubtleOperationTable::find(llvm::StringRef name) {
  for (const SubtleOperationInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef SubtleOperationTable::getName(SubtleOperation value) {
  for (const SubtleOperationInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("subtle operation missing from the table");
}

}
