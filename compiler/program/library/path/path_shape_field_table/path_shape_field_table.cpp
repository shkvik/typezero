#include "compiler/program/library/path/path_shape_field_table/path_shape_field_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const PathShapeFieldInfo kRows[] = {
    {
        .name = "root",
        .value = PathShapeField::ROOT,
    },
    {
        .name = "dir",
        .value = PathShapeField::DIRECTORY,
    },
    {
        .name = "base",
        .value = PathShapeField::BASE,
    },
    {
        .name = "ext",
        .value = PathShapeField::EXTENSION,
    },
    {
        .name = "name",
        .value = PathShapeField::NAME,
    },
};

}

std::optional<PathShapeField> PathShapeFieldTable::find(llvm::StringRef name) {
  for (const PathShapeFieldInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef PathShapeFieldTable::getName(PathShapeField value) {
  for (const PathShapeFieldInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("path shape field missing from the table");
}

}
