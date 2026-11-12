#include "compiler/program/library/builtin_names/internal_name_table/internal_name_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const InternalNameInfo kRows[] = {
    {
        .name = "init",
        .value = InternalName::FIELD_INIT,
    },
};

}

std::optional<InternalName> InternalNameTable::find(llvm::StringRef name) {
  for (const InternalNameInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef InternalNameTable::getName(InternalName value) {
  for (const InternalNameInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("internal name missing from the table");
}

}
