#include "compiler/program/library/crypto/subtle_key_usage_table/subtle_key_usage_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const SubtleKeyUsageInfo kRows[] = {
    {
        .name = "encrypt",
        .value = SubtleKeyUsage::ENCRYPT,
    },
    {
        .name = "decrypt",
        .value = SubtleKeyUsage::DECRYPT,
    },
    {
        .name = "sign",
        .value = SubtleKeyUsage::SIGN,
    },
    {
        .name = "verify",
        .value = SubtleKeyUsage::VERIFY,
    },
    {
        .name = "deriveKey",
        .value = SubtleKeyUsage::DERIVE_KEY,
    },
    {
        .name = "deriveBits",
        .value = SubtleKeyUsage::DERIVE_BITS,
    },
    {
        .name = "wrapKey",
        .value = SubtleKeyUsage::WRAP_KEY,
    },
    {
        .name = "unwrapKey",
        .value = SubtleKeyUsage::UNWRAP_KEY,
    },
};

}

std::optional<SubtleKeyUsage> SubtleKeyUsageTable::find(llvm::StringRef name) {
  for (const SubtleKeyUsageInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef SubtleKeyUsageTable::getName(SubtleKeyUsage value) {
  for (const SubtleKeyUsageInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("subtle key usage missing from the table");
}

std::vector<llvm::StringRef> SubtleKeyUsageTable::getNames() {
  auto names = std::vector<llvm::StringRef>();
  for (const SubtleKeyUsageInfo &info : kRows) {
    names.push_back(info.name);
  }

  return names;
}

}
