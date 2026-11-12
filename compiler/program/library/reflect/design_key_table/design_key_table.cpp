#include "compiler/program/library/reflect/design_key_table/design_key_table.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kDesignPrefix = "design:";

const DesignKeyInfo kRows[] = {
    {
        .name = "design:type",
        .value = DesignKey::TYPE,
    },
    {
        .name = "design:returntype",
        .value = DesignKey::RETURN_TYPE,
    },
    {
        .name = "design:paramtypes",
        .value = DesignKey::PARAM_TYPES,
    },
};

}

llvm::StringRef DesignKeyTable::getName(DesignKey value) {
  for (const DesignKeyInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }

  return "";
}

std::optional<DesignKey> DesignKeyTable::find(llvm::StringRef name) {
  for (const DesignKeyInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef DesignKeyTable::getPrefix() {
  return kDesignPrefix;
}

}
