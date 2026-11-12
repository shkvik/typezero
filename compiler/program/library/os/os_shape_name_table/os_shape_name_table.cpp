#include "compiler/program/library/os/os_shape_name_table/os_shape_name_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const OsShapeNameInfo kRows[] = {
    {
        .name = "CpuInfo",
        .value = OsShapeName::CPU_INFO,
    },
    {
        .name = "UserInfo",
        .value = OsShapeName::USER_INFO,
    },
};

}

std::optional<OsShapeName> OsShapeNameTable::find(llvm::StringRef name) {
  for (const OsShapeNameInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef OsShapeNameTable::getName(OsShapeName value) {
  for (const OsShapeNameInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("os shape name missing from the table");
}

}
