#include "compiler/program/library/crypto/subtle_key_format_table/subtle_key_format_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const SubtleKeyFormatInfo kRows[] = {
    {
        .name = "raw",
        .value = SubtleKeyFormat::RAW,
    },
    {
        .name = "pkcs8",
        .value = SubtleKeyFormat::PKCS8,
    },
    {
        .name = "spki",
        .value = SubtleKeyFormat::SPKI,
    },
    {
        .name = "jwk",
        .value = SubtleKeyFormat::JWK,
    },
};

}

std::optional<SubtleKeyFormat> SubtleKeyFormatTable::find(llvm::StringRef name) {
  for (const SubtleKeyFormatInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef SubtleKeyFormatTable::getName(SubtleKeyFormat value) {
  for (const SubtleKeyFormatInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("subtle key format missing from the table");
}

}
