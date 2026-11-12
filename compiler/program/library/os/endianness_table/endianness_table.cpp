#include "compiler/program/library/os/endianness_table/endianness_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const EndiannessInfo kRows[] = {
    {
        .name = "LE",
        .value = Endianness::LITTLE,
    },
    {
        .name = "BE",
        .value = Endianness::BIG,
    },
};

}

std::optional<Endianness> EndiannessTable::find(llvm::StringRef name) {
  for (const EndiannessInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef EndiannessTable::getName(Endianness value) {
  for (const EndiannessInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("endianness missing from the table");
}

}
