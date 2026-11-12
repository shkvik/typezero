#include "compiler/program/library/iterator/iterator_result_field_table/iterator_result_field_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const IteratorResultFieldInfo kRows[] = {
    {
        .name = "done",
        .value = IteratorResultField::DONE,
    },
    {
        .name = "value",
        .value = IteratorResultField::VALUE,
    },
};

}

std::optional<IteratorResultField> IteratorResultFieldTable::find(llvm::StringRef name) {
  for (const IteratorResultFieldInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef IteratorResultFieldTable::getName(IteratorResultField value) {
  for (const IteratorResultFieldInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("iterator result field missing from the table");
}

}
