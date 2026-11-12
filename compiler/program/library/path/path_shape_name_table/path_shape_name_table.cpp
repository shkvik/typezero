#include "compiler/program/library/path/path_shape_name_table/path_shape_name_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const PathShapeNameInfo kRows[] = {
    {
        .name = "ParsedPath",
        .value = PathShapeName::PARSED_PATH,
    },
    {
        .name = "FormatInputPathObject",
        .value = PathShapeName::FORMAT_INPUT_PATH_OBJECT,
    },
};

}

std::optional<PathShapeName> PathShapeNameTable::find(llvm::StringRef name) {
  for (const PathShapeNameInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef PathShapeNameTable::getName(PathShapeName value) {
  for (const PathShapeNameInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("path shape name missing from the table");
}

}
